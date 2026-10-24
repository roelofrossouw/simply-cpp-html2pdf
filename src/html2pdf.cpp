#include "html2pdf.h"

#include <pdflite/document.h>
#include <style.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
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
            std::string text;
            double top;
        };
    }

    struct html2pdf::impl {
        options settings;
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

        // A marker's text for #tag# on a page: the first on the latest page up to it that has one.
        [[nodiscard]] std::string variable(const std::string &tag, const unsigned page) const {
            std::string value;
            unsigned found_on = 0;
            for (const auto &each: markers) {
                if (each.tag != tag) continue;
                const unsigned on = page_of_content(cut_at(cuts, each.top));
                if (on > page) break;
                if (on != found_on) {
                    value = each.text;
                    found_on = on;
                }
            }
            return value;
        }

        [[nodiscard]] std::string filled(std::string html, const unsigned page) const {
            replace_all(html, "#Page#", std::to_string(page));
            replace_all(html, "#ToPage#", std::to_string(page_count()));
            for (const std::string tag: {"section", "subsection", "subtitle"}) {
                const std::string name = "#" + tag + "#";
                if (html.find(name) == std::string::npos) continue;
                auto value = variable(tag, page);
                if (value.ends_with(".png") || value.ends_with(".jpg")) value = "<img src=\"" + value + "\" />";
                replace_all(html, name, value);
            }
            return html;
        }

        [[nodiscard]] web::html header_or_footer(const std::string &html, const unsigned page) const {
            auto options = settings.html;
            options.css = settings.css + (page == 1 ? first_page_css : other_page_css);
            web::html document{prepared(filled(html, page)), options};
            document.layout(width);
            return document;
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

        impl(const std::string_view html, options given) : settings(std::move(given)) {
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
            content.emplace(prepared(std::string{html}), options);
            content->layout(width);
            recorded = content->drawing();
            recorded_sizes = measured_of(recorded);
            for (const auto &element: content->select("section, subsection, subtitle")) {
                auto text = element.attribute("alt").value_or("");
                if (text.empty()) text = element.text();
                markers.push_back({element.name(), text, element.box().top()});
            }

            // The table of contents' length moves the pages after it, which its entries number:
            // laid out until it stays the same length.
            toc_start = settings.toc_page;
            paginate_content();
            if (toc_start) {
                toc_start = std::min<unsigned>(toc_start, static_cast<unsigned>(cuts.size()) + 1);
                for (int round = 0; round < 5; ++round) {
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

            for (unsigned page = 1; page <= page_count(); ++page) {
                file->set_page(page);
                const double left = points(settings.margins.left);
                const auto fixed_at = [&](const double top) {
                    return [=](const double x, const double y) -> std::optional<placed> {
                        return placed{page, left + x * points_per_px, top + y * points_per_px};
                    };
                };
                if (!settings.header.empty()) {
                    auto header = header_or_footer(settings.header, page);
                    header.draw(*file, transform::scale(points_per_px).then(transform::translate(left, margin_top(page))));
                    add_links(*file, header, fixed_at(margin_top(page)), *content);
                }
                if (!settings.footer.empty()) {
                    auto bottom = header_or_footer(settings.footer, page);
                    const double top = page_height - points(settings.margins.bottom) - footer * points_per_px;
                    bottom.draw(*file, transform::scale(points_per_px).then(transform::translate(left, top)));
                    add_links(*file, bottom, fixed_at(top), *content);
                }
                if (settings.draw_margins) draw_margins(*file, page);
            }

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

    html2pdf::html2pdf(const std::string_view html, options settings) : impl_(std::make_unique<impl>(html, std::move(settings))) {
    }

    html2pdf::html2pdf(html2pdf &&) noexcept = default;
    html2pdf &html2pdf::operator=(html2pdf &&) noexcept = default;
    html2pdf::~html2pdf() = default;

    unsigned html2pdf::page_count() const { return impl_->page_count(); }

    const std::vector<html2pdf::cut> &html2pdf::cuts() const { return impl_->cuts; }

    void html2pdf::save(const std::filesystem::path &file) const { impl_->save(file); }

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
