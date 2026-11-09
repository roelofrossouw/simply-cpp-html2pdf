#include "html2pdf.h"

#include <pdflite/document.h>
#include <style.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include <unistd.h>

namespace sc {
    namespace {
        constexpr double points_per_px = 0.75;

        double points(const double mm) { return mm * 72 / 25.4; }

        // The markers are invisible: they only mark where sections start.
        const std::string marker_css = " section, subsection, subtitle { display: block; max-height: 0; overflow: hidden; color: transparent; }";
        const std::string first_page_css = " #POHead { display: none; } #FakeHeaders { display: none; }";
        const std::string other_page_css = " #P1Head { display: none; }";

        void replace_all(std::string &text, const std::string_view from, const std::string_view to) {
            for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
        }

        // litehtml doesn't size columns by <colgroup>: its <col>s become a row of cells that do.
        std::string prepared(std::string html) {
            replace_all(html, "<colgroup>", "<tr>");
            replace_all(html, "<col ", "<td ");
            replace_all(html, "</colgroup>", "</tr>");
            return html;
        }

        bool styled(const web::style &style, const std::string_view property, const std::initializer_list<std::string_view> values) {
            const auto value = style.value(property);
            return value && std::ranges::any_of(values, [&](const std::string_view wanted) { return *value == wanted; });
        }

        rect bounds_of(const drawing::item &item) {
            return std::visit([](const auto &thing) { return thing.bounds(); }, item);
        }

        // Text and images are cut between, never through.
        bool atomic(const drawing::item &item) { return !std::holds_alternative<drawable>(item); }

        // So are boxes (backgrounds, borders, a vector image's shapes) that fit on a page, unless
        // they may be split.
        bool whole(const drawing::item &item, const rect &box, const bool split_boxes, const double page) {
            if (atomic(item)) return true;
            return !split_boxes && box.height() <= page;
        }

        // What a page may not be cut through, and how far past a cut it may reach (reach_of()).
        struct kept {
            double top, bottom, reach = 1.0;
        };

        // How far an item may reach past a cut and stay on its page: a line of text its descender
        // (below its line box when the line height is tight: no reason to move the line to the
        // next page), a box a pixel (litehtml rounds boxes to whole pixels, so a table row's border
        // box can reach a fraction past where the next element starts).
        double reach_of(const drawing::item &item, const rect &box) {
            if (std::holds_alternative<label>(item)) return 0.3 * box.height();
            return 1.0;
        }

        // A recorded item's bounds and reach, measured once rather than for every page (text is
        // measured in its font, which is most of the time a long document takes otherwise).
        struct measured {
            rect box;
            double reach;
        };

        std::vector<measured> measured_of(const drawing &picture) {
            std::vector<measured> out;
            out.reserve(picture.items().size());
            for (const auto &item: picture.items()) {
                const rect box = bounds_of(item);
                out.push_back({box, reach_of(item, box)});
            }
            return out;
        }

        // A section, subsection or subtitle: where it is, and the text it stands for.
        struct marker {
            std::string tag;
            std::string text;  // its alt, else its text: the table of contents' and outline's entry
            std::string image; // its text when that's an image's path (logo.png): what #tag# shows
            double top;
        };

        // Whether text is an image's path or URL: one word ending in .png, .jpg, .jpeg, .gif or .svg.
        bool image_path(const std::string_view text) {
            if (text.empty() || std::ranges::any_of(text, [](const char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; })) return false;
            std::string lower{text};
            for (auto &c: lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return std::ranges::any_of(std::initializer_list<std::string_view>{".png", ".jpg", ".jpeg", ".gif", ".svg"},
                                       [&](const std::string_view end) { return lower.ends_with(end); });
        }

        std::string trimmed(const std::string_view text) {
            const auto start = text.find_first_not_of(" \t\r\n");
            if (start == std::string_view::npos) return {};
            return std::string{text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1)};
        }

        // text for an HTML attribute's value in double quotes.
        std::string attribute_text(const std::string_view text) {
            std::string out;
            for (const char c: text) {
                if (c == '&') out += "&amp;";
                else if (c == '"') out += "&quot;";
                else if (c == '<') out += "&lt;";
                else out += c;
            }
            return out;
        }
    }

    struct html2pdf::impl {
        options settings;
        std::function<void()> check; // called now and then; throws to stop

        void checked() const {
            if (check) check();
        }
        double page_width = 0, page_height = 0; // points
        double width = 0;                        // the content's, px
        double header_first = 0, header_other = 0, footer = 0; // heights, px

        std::optional<web::html> content;
        drawing recorded;
        std::vector<measured> recorded_sizes;
        std::vector<cut> cuts;
        std::vector<marker> markers;

        // The table of contents, when there's one.
        std::optional<web::html> toc;
        drawing toc_recorded;
        std::vector<measured> toc_sizes;
        std::vector<cut> toc_cuts;
        unsigned toc_start = 0; // the page it starts on; 0 for none

        [[nodiscard]] double margin_top(const unsigned page) const {
            return points(page == 1 && settings.margins.first_top ? *settings.margins.first_top : settings.margins.top);
        }

        [[nodiscard]] double header_height(const unsigned page) const { return (page == 1 ? header_first : header_other) * points_per_px; }

        // Where the content goes on a page, in points.
        [[nodiscard]] rect content_area(const unsigned page) const {
            const double top = margin_top(page) + header_height(page);
            const double bottom = page_height - points(settings.margins.bottom) - footer * points_per_px;
            return {points(settings.margins.left), top, width * points_per_px, std::max(1.0, bottom - top)};
        }

        [[nodiscard]] unsigned toc_count() const { return static_cast<unsigned>(toc_cuts.size()); }

        // The page content page i (from 0) lands on, the table of contents' pages counted.
        [[nodiscard]] unsigned page_of_content(const std::size_t i) const {
            const auto page = static_cast<unsigned>(i + 1);
            return toc_start && page >= toc_start ? page + toc_count() : page;
        }

        [[nodiscard]] unsigned page_of_toc(const std::size_t i) const { return static_cast<unsigned>(toc_start + i); }

        [[nodiscard]] unsigned page_count() const { return static_cast<unsigned>(cuts.size()) + toc_count(); }

        // The content page holding y (px), from 0.
        [[nodiscard]] static std::size_t cut_at(const std::vector<cut> &pages, const double y) {
            for (std::size_t i = 0; i < pages.size(); ++i)
                if (y < pages[i].bottom - 1e-6) return i;
            return pages.empty() ? 0 : pages.size() - 1;
        }

        // Cuts a recorded document into pages, its page i (from 0) landing on page_of(i): each page
        // ends at the next hard break, or else as low as it can without going through text, an
        // image or a box kept whole.
        template<typename PageOf>
        std::vector<cut> paginate(const drawing &picture, const std::vector<measured> &sizes, const double total,
                                  std::vector<double> breaks, std::vector<kept> whole, PageOf page_of) const {
            const auto height = [&](const std::size_t i) { return content_area(page_of(i)).height() / points_per_px; };
            const double tallest = std::max(height(0), height(1)); // a box taller than a page is cut after all
            const auto &items = picture.items();
            for (std::size_t i = 0; i < items.size(); ++i)
                if (sc::whole(items[i], sizes[i].box, settings.split_boxes, tallest))
                    whole.push_back({sizes[i].box.top(), sizes[i].box.bottom(), sizes[i].reach});
            std::ranges::sort(breaks);
            std::vector<cut> out;
            double start = 0;
            for (unsigned page = 0; start < total - 0.5; ++page) {
                // At the next hard break, unless there's nothing above it on the page.
                double hard = start + height(page);
                for (const double at: breaks)
                    if (at > start + 0.5 && at < hard && std::ranges::any_of(whole, [&](const kept &box) { return box.top >= start - 0.01 && box.top < at; })) {
                        hard = at;
                        break;
                    }
                double end = hard;
                for (bool moved = true; moved;) {
                    moved = false;
                    for (const auto &box: whole)
                        if (box.top > start + 0.5 && box.top < end && box.bottom > end + box.reach) {
                            end = box.top;
                            moved = true;
                        }
                }
                if (end <= start + 0.5) end = hard; // what won't fit on any page is cut after all
                out.push_back({start, std::min(end, total)});
                start = end;
            }
            if (out.empty()) out.push_back({0, total});
            return out;
        }

        // The tops of what starts a page, and the boxes kept whole, in a document.
        static std::pair<std::vector<double>, std::vector<kept> > breaks_of(const web::html &document) {
            std::vector<double> breaks;
            std::vector<kept> whole;
            for (const auto &element: document.select("section, [style]")) {
                const web::style style{element.attribute("style").value_or("")};
                const rect box = element.box();
                if (element.name() == "section" || styled(style, "page-break-before", {"always", "page"}) ||
                    styled(style, "break-before", {"page", "always"}))
                    breaks.push_back(box.top());
                if (styled(style, "page-break-after", {"always", "page"}) || styled(style, "break-after", {"page", "always"}))
                    breaks.push_back(box.bottom());
                if (styled(style, "page-break-inside", {"avoid"}) || styled(style, "break-inside", {"avoid", "avoid-page"}))
                    whole.push_back({box.top(), box.bottom()});
            }
            return {breaks, whole};
        }

        static double total_of(const web::html &document, const drawing &picture) {
            return std::max(document.height(), picture.empty() ? 0.0 : picture.bounds().bottom());
        }

        void paginate_content() {
            const auto [breaks, whole] = breaks_of(*content);
            const double total = total_of(*content, recorded);
            cuts = paginate(recorded, recorded_sizes, total, breaks, whole, [&](const std::size_t i) { return page_of_content(i); });
        }

        // What #tag# shows on a page, from the first marker on the latest page up to it that has
        // one: its image when its text is an image's path (whatever its alt), else its text.
        [[nodiscard]] std::string variable(const std::string &tag, const unsigned page) const {
            const marker *found = nullptr;
            unsigned found_on = 0;
            for (const auto &each: markers) {
                if (each.tag != tag) continue;
                const unsigned on = page_of_content(cut_at(cuts, each.top));
                if (on > page) break;
                if (on != found_on) {
                    found = &each;
                    found_on = on;
                }
            }
            if (!found) return {};
            // As wide as the place it's in at most; the caller's CSS may size it further (img.section-image).
            if (!found->image.empty())
                return "<img class=\"section-image\" src=\"" + attribute_text(found->image) + "\" style=\"max-width: 100%\" />";
            return found->text;
        }

        [[nodiscard]] std::string filled(std::string html, const unsigned page) const { return filled(std::move(html), page, std::to_string(page)); }

        // The same, #Page# as number (a placeholder, for a layout used again: laid_out_part()).
        [[nodiscard]] std::string filled(std::string html, const unsigned page, const std::string &number) const {
            replace_all(html, "#Page#", number);
            replace_all(html, "#ToPage#", std::to_string(page_count()));
            for (const std::string tag: {"section", "subsection", "subtitle"}) {
                const std::string name = "#" + tag + "#";
                if (html.find(name) == std::string::npos) continue;
                replace_all(html, name, variable(tag, page));
            }
            return html;
        }

        [[nodiscard]] web::html header_or_footer(const std::string &html, const unsigned page) const {
            return laid_out(filled(html, page), page);
        }

        // HTML filled in for a page, as a header or footer laid out.
        [[nodiscard]] web::html laid_out(const std::string &text, const unsigned page) const {
            auto options = settings.html;
            options.css = settings.css + (page == 1 ? first_page_css : other_page_css);
            web::html document{prepared(text), options};
            document.layout(width);
            return document;
        }

        // A header or footer laid out once and drawn on every page that shows the same: the same
        // #section#, #subsection# and #subtitle#, a page number of the same length, and the first
        // page's or the others' CSS. Laying it out again for every page was most of save()'s time
        // (a 34 kB stylesheet parsed twice a page). The page number is laid out as a placeholder of
        // as many digits and put in the drawing afterwards: the same width when the font's digits all
        // are (most fonts', and the built-in fonts'). Where that can't be relied on, it isn't reused.
        struct part {
            std::optional<web::html> document;
            drawing picture;
            std::string placeholder; // what stands for the page number in its words; empty: none
            bool reusable = false;
        };

        // The digit text doesn't show (outside tags and entities), to stand for the page number;
        // '0' when every digit shows (and the count in reusable_part() then decides).
        static char unused_digit(const std::string_view html) {
            bool used[10] = {};
            bool in_tag = false, in_entity = false;
            for (const char c: html) {
                if (in_tag) {
                    in_tag = c != '>';
                } else if (in_entity) {
                    in_entity = c != ';' && !std::isspace(static_cast<unsigned char>(c));
                } else if (c == '<') {
                    in_tag = true;
                } else if (c == '&') {
                    in_entity = true;
                } else if (c >= '0' && c <= '9') {
                    used[c - '0'] = true;
                }
            }
            for (int d = 0; d < 10; ++d)
                if (!used[d]) return static_cast<char>('0' + d);
            return '0';
        }

        static std::size_t occurrences(const std::string_view text, const std::string_view of) {
            std::size_t count = 0;
            for (auto at = text.find(of); at != std::string_view::npos; at = text.find(of, at + of.size())) ++count;
            return count;
        }

        // Whether a font's digits are all as wide (so any number of a length lays out the same).
        bool tabular(const sc::font &font) const {
            if (settings.builtin_fonts) return true; // the built-in fonts' digits are (Adobe's metrics)
            const double zero = font.width("0");
            for (const char *digit: {"1", "2", "3", "4", "5", "6", "7", "8", "9"})
                if (std::abs(font.width(digit) - zero) > 1e-6) return false;
            return true;
        }

        // The header or footer html for a page: its drawing (in px, from its top left), and the
        // document (for its links), from parts when laid out before.
        std::pair<drawing, const web::html *> part_for(std::map<std::string, part> &parts, const std::string &html, const unsigned page,
                                                       std::optional<web::html> &alone) const {
            const std::string number = std::to_string(page);
            const auto pages_in = occurrences(html, "#Page#");
            // #Page# marked first (a control character no HTML has), then made the placeholder.
            const std::string mark(number.size(), '\x01');
            std::string with_placeholder = filled(html, page, mark);
            const char digit = unused_digit(with_placeholder);
            const std::string placeholder(number.size(), digit);
            replace_all(with_placeholder, mark, placeholder);
            const std::string key = std::string{page == 1 ? "1:" : "n:"} + with_placeholder;
            auto [found, made] = parts.try_emplace(key);
            part &each = found->second;
            if (made) {
                each.document.emplace(laid_out(with_placeholder, page));
                each.picture = each.document->drawing();
                each.placeholder = pages_in ? placeholder : std::string{};
                // Reused only when the placeholder is found as often as #Page# is in the html, and
                // only in words of fonts whose digits are all as wide.
                std::size_t seen = 0;
                bool fits = true;
                for (const auto &item: each.picture.items()) {
                    if (const auto *words = std::get_if<label>(&item); words && pages_in) {
                        const auto here = occurrences(words->text.content, placeholder);
                        seen += here;
                        if (here && !tabular(words->text.font)) fits = false;
                    }
                }
                each.reusable = fits && seen == pages_in;
            }
            if (!each.reusable) {
                alone.emplace(laid_out(filled(html, page), page));
                return {alone->drawing(), &*alone};
            }
            drawing out = each.picture;
            if (!each.placeholder.empty()) {
                drawing numbered;
                for (auto item: out.items()) {
                    if (auto *words = std::get_if<label>(&item)) replace_all(words->text.content, each.placeholder, number);
                    numbered.add(std::move(item));
                }
                numbered.page(out.page());
                out = std::move(numbered);
            }
            return {std::move(out), &*each.document};
        }

        [[nodiscard]] std::string toc_html() const {
            std::string out = "<div class=\"toc\"><h1 class=\"title\">" + settings.toc_title + "</h1>";
            for (const auto &each: markers) {
                if (each.tag == "subtitle") {
                    out += "<h2>" + each.text + "</h2>";
                } else if (each.tag == "section") {
                    const auto page = std::to_string(page_of_content(cut_at(cuts, each.top)));
                    out += "<p><a href=\"#page=" + page + "\">" + each.text + "</a><span style=\"float: right\">Page " + page + "</span></p>";
                }
            }
            return out + "</div>";
        }

        void paginate_toc() {
            auto options = settings.html;
            options.css = settings.css + marker_css;
            toc.emplace(toc_html(), options);
            toc->layout(width);
            toc_recorded = toc->drawing();
            toc_sizes = measured_of(toc_recorded);
            const auto [breaks, whole] = breaks_of(*toc);
            toc_cuts = paginate(toc_recorded, toc_sizes, total_of(*toc, toc_recorded), breaks, whole,
                                [&](const std::size_t i) { return page_of_toc(i); });
        }

        impl(const std::string_view html, options given, std::function<void()> checking) : settings(std::move(given)), check(std::move(checking)) {
            checked();
            settings.html.builtin_fonts = settings.builtin_fonts;
            if (!settings.html.images) settings.html.images.emplace(); // the header and footer, laid out for every page, read their images once
            const auto page = page_dimensions(settings.size, settings.turned);
            page_width = page.width();
            page_height = page.height();
            width = (page_width - points(settings.margins.left) - points(settings.margins.right)) / points_per_px;
            if (width <= 0) throw std::invalid_argument("sc::html2pdf: the margins leave no room for the content");

            // The header's and footer's heights, before their variables are known.
            if (!settings.header.empty()) {
                header_first = header_or_footer(settings.header, 1).height();
                header_other = header_or_footer(settings.header, 2).height();
            }
            if (!settings.footer.empty()) footer = header_or_footer(settings.footer, 1).height();

            auto options = settings.html;
            options.css = settings.css + marker_css;
            checked();
            content.emplace(prepared(std::string{html}), options);
            checked();
            content->layout(width);
            checked();
            recorded = content->drawing();
            recorded_sizes = measured_of(recorded);
            checked();
            for (const auto &element: content->select("section, subsection, subtitle")) {
                const auto own = trimmed(element.text());
                auto text = element.attribute("alt").value_or("");
                if (text.empty()) text = own;
                markers.push_back({element.name(), text, image_path(own) ? own : std::string{}, element.box().top()});
            }

            // The table of contents' length moves the pages after it, which its entries number:
            // laid out until it stays the same length.
            toc_start = settings.toc_page;
            paginate_content();
            if (toc_start) {
                toc_start = std::min<unsigned>(toc_start, static_cast<unsigned>(cuts.size()) + 1);
                for (int round = 0; round < 5; ++round) {
                    checked();
                    const auto before = toc_count();
                    paginate_toc();
                    paginate_content();
                    if (toc_count() == before) break;
                }
                paginate_toc(); // numbered by the final cuts
            }
        }

        template<typename PageOf>
        void draw_pages(pdf &file, const drawing &picture, const std::vector<measured> &sizes, const std::vector<cut> &pages,
                        PageOf page_of) const {
            for (std::size_t i = 0; i < pages.size(); ++i) {
                checked();
                const unsigned page = page_of(i);
                file.set_page(page);
                draw_part(file, picture, sizes, pages[i], content_area(page));
            }
        }

        // The part of a recorded document a page shows, placed on it and clipped to its area.
        static void draw_part(pdf &file, const drawing &picture, const std::vector<measured> &sizes, const cut &part, const rect &area) {
            drawing shown;
            double below = 0; // how far a line's descender reaches past the page's cut
            const auto &items = picture.items();
            for (std::size_t i = 0; i < items.size(); ++i) {
                const auto &[box, reach] = sizes[i];
                if (box.bottom() <= part.top + 0.01 || box.top() >= part.bottom) continue;
                // What reaches only a little past a cut is on the page its top is on, all of it.
                if (box.top() < part.top - 0.01 && box.bottom() <= part.top + reach) continue;
                if (box.top() < part.bottom && box.bottom() <= part.bottom + reach) below = std::max(below, box.bottom() - part.bottom);
                shown.add(items[i]);
            }
            shown.apply(transform::translate(0, -part.top).then(transform::scale(points_per_px)).then(transform::translate(area.left(), area.top())));
            file.push_clip(to_path(rect{area.left(), area.top(), area.width(), (part.bottom - part.top + below) * points_per_px}));
            file.draw(shown);
            file.pop_clip();
        }

        // Where y (px) in a document cut into pages lands: its page and the point on it.
        struct placed {
            unsigned page;
            double x, y;
        };

        template<typename PageOf>
        [[nodiscard]] placed place(const std::vector<cut> &pages, PageOf page_of, const double x, const double y) const {
            const auto i = cut_at(pages, y);
            const unsigned page = page_of(i);
            const rect area = content_area(page);
            return {page, area.left() + x * points_per_px, area.top() + (y - pages[i].top) * points_per_px};
        }

        [[nodiscard]] std::optional<placed> content_place(const double x, const double y) const {
            return place(cuts, [&](const std::size_t i) { return page_of_content(i); }, x, y);
        }

        [[nodiscard]] std::optional<placed> toc_place(const double x, const double y) const {
            if (toc_cuts.empty()) return std::nullopt;
            return place(toc_cuts, [&](const std::size_t i) { return page_of_toc(i); }, x, y);
        }

        // The links in a document, where(x, y) placing a point of it (px) on its page.
        void add_links(pdf &file, const web::html &document, const std::function<std::optional<placed>(double, double)> &where,
                       const web::html &targets) const {
            for (const auto &link: document.select("a[href]")) {
                const auto href = link.attribute("href").value_or("");
                const rect box = link.box();
                const auto at = where(box.left(), box.top());
                if (href.empty() || !at || box.width() <= 0 || box.height() <= 0) continue;
                file.set_page(at->page);
                const rect area{at->x, at->y, box.width() * points_per_px, box.height() * points_per_px};
                if (href.starts_with("#page=")) {
                    try {
                        const auto target = std::stoul(href.substr(6));
                        if (target >= 1 && target <= page_count()) file.add_link(static_cast<unsigned>(target), area);
                    } catch (const std::exception &) {
                        // not a number: no link
                    }
                } else if (href.starts_with('#')) {
                    const auto id = href.substr(1);
                    auto found = targets.select("[id=\"" + id + "\"]");
                    if (found.empty()) found = targets.select("a[name=\"" + id + "\"]");
                    if (found.empty()) continue;
                    if (const auto to = content_place(found.front().box().left(), found.front().box().top()))
                        file.add_link(to->page, area, std::max(0.0, to->y - 4));
                } else {
                    file.add_link(web::html::resolve(href, settings.html.base_url), area);
                }
            }
        }

        void add_outline(pdf &file) const {
            bool titled = false;
            for (const auto &each: markers) {
                if (each.tag == "subsection") continue;
                const auto at = content_place(0, each.top);
                if (each.tag == "subtitle") titled = true;
                file.add_outline(each.text, at->page, at->y, each.tag == "section" && titled ? 1 : 0);
            }
            if (markers.empty()) {
                for (const auto &heading: content->select("h1, h2, h3")) {
                    const auto at = content_place(0, heading.box().top());
                    file.add_outline(heading.text(), at->page, at->y, heading.name()[1] - '1');
                }
            }
        }

        void draw_margins(pdf &file, const unsigned page) const {
            const sc::stroke red{sc::color{0.65, 0, 0}, 0.5}, green{sc::color{0.5, 0.65, 0}, 0.5};
            const double left = points(settings.margins.left), right = page_width - points(settings.margins.right);
            const double top = margin_top(page), bottom = page_height - points(settings.margins.bottom);
            file.draw(rect{left, top, right - left, bottom - top}, red);
            if (header_height(page) > 0) file.draw(rect{left, top, right - left, header_height(page)}, green);
            if (footer > 0) file.draw(rect{left, bottom - footer * points_per_px, right - left, footer * points_per_px}, green);
        }

        void save(const std::filesystem::path &path) const {
            checked();
            std::optional<pdf> file;
            if (settings.protection) file.emplace(path, settings.size, settings.turned, *settings.protection);
            else file.emplace(path, settings.size, settings.turned);
            file->builtin_fonts(settings.builtin_fonts);
            file->image_resolution(settings.image_resolution);
            file->set_page(page_count());

            draw_pages(*file, recorded, recorded_sizes, cuts, [&](const std::size_t i) { return page_of_content(i); });
            draw_pages(*file, toc_recorded, toc_sizes, toc_cuts, [&](const std::size_t i) { return page_of_toc(i); });
            add_links(*file, *content, [&](const double x, const double y) { return content_place(x, y); }, *content);
            if (toc) add_links(*file, *toc, [&](const double x, const double y) { return toc_place(x, y); }, *content);

            std::map<std::string, part> headers, footers; // laid out once for every page that shows the same
            for (unsigned page = 1; page <= page_count(); ++page) {
                checked();
                file->set_page(page);
                const double left = points(settings.margins.left);
                const auto fixed_at = [&](const double top) {
                    return [=](const double x, const double y) -> std::optional<placed> {
                        return placed{page, left + x * points_per_px, top + y * points_per_px};
                    };
                };
                if (!settings.header.empty()) {
                    std::optional<web::html> alone;
                    auto [picture, header] = part_for(headers, settings.header, page, alone);
                    picture.apply(transform::scale(points_per_px).then(transform::translate(left, margin_top(page))));
                    file->draw(picture);
                    add_links(*file, *header, fixed_at(margin_top(page)), *content);
                }
                if (!settings.footer.empty()) {
                    std::optional<web::html> alone;
                    auto [picture, bottom] = part_for(footers, settings.footer, page, alone);
                    const double top = page_height - points(settings.margins.bottom) - footer * points_per_px;
                    picture.apply(transform::scale(points_per_px).then(transform::translate(left, top)));
                    file->draw(picture);
                    add_links(*file, *bottom, fixed_at(top), *content);
                }
                if (settings.draw_margins) draw_margins(*file, page);
            }

            checked();
            if (settings.outline) add_outline(*file);
            auto metadata = settings.metadata;
            if (!metadata.contains("Title") && !content->title().empty()) metadata["Title"] = content->title();
            if (!metadata.contains("Producer")) metadata["Producer"] = "simply-cpp-html2pdf " SC_HTML2PDF_VERSION " (simply-cpp-pdflite " + std::string{pdflite::version()} + ")";
            for (const auto &[key, value]: metadata) file->metadata(key, value);
            file->close();
        }
    };

    html2pdf::html2pdf(const std::string_view html) : html2pdf(html, options{}) {
    }

    html2pdf::html2pdf(const std::string_view html, options settings) : html2pdf(html, std::move(settings), {}) {
    }

    html2pdf::html2pdf(const std::string_view html, options settings, std::function<void()> check)
        : impl_(std::make_unique<impl>(html, std::move(settings), std::move(check))) {
    }

    html2pdf::html2pdf(html2pdf &&) noexcept = default;
    html2pdf &html2pdf::operator=(html2pdf &&) noexcept = default;
    html2pdf::~html2pdf() = default;

    unsigned html2pdf::page_count() const { return impl_->page_count(); }

    const std::vector<html2pdf::cut> &html2pdf::cuts() const { return impl_->cuts; }

    void html2pdf::save(const std::filesystem::path &file) const {
        try {
            impl_->save(file);
        } catch (...) {
            // Stopped part way (a check that threw, a write that failed): no half-written file.
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
            throw;
        }
    }

    std::string html2pdf::bytes() const {
        // sc::pdf only writes files.
        std::string name = (std::filesystem::temp_directory_path() / "sc-html2pdf-XXXXXX").string();
        const int descriptor = mkstemp(name.data());
        if (descriptor < 0) throw std::runtime_error("sc::html2pdf: can't make a temporary file in " + std::filesystem::temp_directory_path().string());
        ::close(descriptor);
        const std::filesystem::path path{name};
        struct removed {
            const std::filesystem::path &path;
            ~removed() {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } guard{path};
        impl_->save(path);
        std::ifstream in{path, std::ios::binary};
        if (!in) throw std::runtime_error("sc::html2pdf: can't read back " + name);
        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }
}
