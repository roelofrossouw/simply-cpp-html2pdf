#include "html2pdf.h"

#include <style.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace sc {
    namespace {
        constexpr double points_per_px = 0.75;

        double points(const double mm) { return mm * 72 / 25.4; }

        // The markers are invisible: they only mark where sections start.
        const std::string marker_css = " section, subsection, subtitle { display: block; max-height: 0; overflow: hidden; color: transparent; }";
        // A header's #P1Head shows on the first page only, its #POHead on the others.
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

        bool styled(const web::html::element &element, const std::string_view property, const std::initializer_list<std::string_view> values) {
            const auto style = web::style{element.attribute("style").value_or("")};
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
        bool whole(const drawing::item &item, const bool split_boxes, const double page) {
            if (atomic(item)) return true;
            return !split_boxes && bounds_of(item).height() <= page;
        }

        // What a page may not be cut through: its top and bottom, and how far past a cut it may
        // still reach and stay on the page (a line of text's descender, below its line box when
        // the line height is tight: not a reason to move the line to the next page).
        struct kept {
            double top, bottom, reach = 1.0;
        };

        // How far an item may reach past a cut and stay on its page: a line of text its descender,
        // a box a pixel (litehtml rounds boxes to whole pixels, so a table row's border box can
        // reach a fraction past where the next element starts).
        double reach_of(const drawing::item &item) {
            if (std::holds_alternative<label>(item)) return 0.3 * bounds_of(item).height();
            return 1.0;
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

        // The content: laid out, recorded, cut into pages.
        std::optional<web::html> content;
        drawing recorded;
        std::vector<cut> cuts;
        std::vector<marker> markers;

        // The table of contents, when there's one.
        std::optional<web::html> toc;
        drawing toc_recorded;
        std::vector<cut> toc_cuts;
        unsigned toc_start = 0; // the page it starts on; 0 for none

        // ------------------------------------------------------------ the page's areas

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

        // ------------------------------------------------------------ numbering

        [[nodiscard]] unsigned toc_count() const { return static_cast<unsigned>(toc_cuts.size()); }

        // The page content page i (from 0) lands on, the table of contents' pages counted.
        [[nodiscard]] unsigned page_of_content(const std::size_t i) const {
            const auto page = static_cast<unsigned>(i + 1);
            return toc_start && page >= toc_start ? page + toc_count() : page;
        }

        [[nodiscard]] unsigned page_count() const { return static_cast<unsigned>(cuts.size()) + toc_count(); }

        // The content page holding y (px), from 0.
        [[nodiscard]] static std::size_t cut_at(const std::vector<cut> &pages, const double y) {
            for (std::size_t i = 0; i < pages.size(); ++i)
                if (y < pages[i].bottom - 1e-6) return i;
            return pages.empty() ? 0 : pages.size() - 1;
        }

        // ------------------------------------------------------------ cutting into pages

        // Cuts a recorded document into pages as tall as height(page) says (px), the first being
        // page first: each page ends at the next hard break, or else as low as it can without
        // going through text, an image or a box kept whole.
        template<typename Height>
        std::vector<cut> paginate(const drawing &picture, const double total, std::vector<double> breaks,
                                  std::vector<kept> whole, Height height) const {
            const double tallest = std::max(height(0), height(1)); // a box taller than a page is cut after all
            for (const auto &item: picture.items())
                if (sc::whole(item, settings.split_boxes, tallest)) {
                    const rect box = bounds_of(item);
                    whole.push_back({box.top(), box.bottom(), reach_of(item)});
                }
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
            for (const auto &element: document.select("section, [style]")) {
                if (element.name() == "section" || styled(element, "page-break-before", {"always", "page"}) ||
                    styled(element, "break-before", {"page", "always"}))
                    breaks.push_back(element.box().top());
                if (styled(element, "page-break-after", {"always", "page"}) || styled(element, "break-after", {"page", "always"}))
                    breaks.push_back(element.box().bottom());
            }
            std::vector<kept> whole;
            for (const auto &element: document.select("[style]"))
                if (styled(element, "page-break-inside", {"avoid"}) || styled(element, "break-inside", {"avoid", "avoid-page"}))
                    whole.push_back({element.box().top(), element.box().bottom()});
            return {breaks, whole};
        }

        static double total_of(const web::html &document, const drawing &picture) {
            return std::max(document.height(), picture.empty() ? 0.0 : picture.bounds().bottom());
        }

        void paginate_content() {
            const auto [breaks, whole] = breaks_of(*content);
            const double total = total_of(*content, recorded);
            cuts = paginate(recorded, total, breaks, whole, [&](const unsigned i) { return content_area(page_of_content(i)).height() / points_per_px; });
        }

        // ------------------------------------------------------------ variables

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

        // ------------------------------------------------------------ the table of contents

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
            const auto [breaks, whole] = breaks_of(*toc);
            toc_cuts = paginate(toc_recorded, total_of(*toc, toc_recorded), breaks, whole,
                                [&](const unsigned i) { return content_area(toc_start + i).height() / points_per_px; });
        }

        // ------------------------------------------------------------ laying out

        impl(const std::string_view html, options given) : settings(std::move(given)) {
            settings.html.builtin_fonts = settings.builtin_fonts;
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

        // ------------------------------------------------------------ writing

        // The part of a recorded document a page shows, placed on it and clipped to its area.
        static void draw_part(pdf &file, const drawing &picture, const cut &part, const rect &area) {
            drawing shown;
            double below = 0; // how far a line's descender reaches past the page's cut
            for (const auto &item: picture.items()) {
                const rect box = bounds_of(item);
                if (box.bottom() <= part.top + 0.01 || box.top() >= part.bottom) continue;
                // What reaches only a little past a cut is on the page its top is on, all of it.
                if (box.top() < part.top - 0.01 && box.bottom() <= part.top + reach_of(item)) continue;
                if (box.top() < part.bottom && box.bottom() <= part.bottom + reach_of(item)) below = std::max(below, box.bottom() - part.bottom);
                shown.add(item);
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

        // The links in a document drawn at (left, top) on page (px to points), or cut into pages.
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

        [[nodiscard]] std::optional<placed> content_place(const double x, const double y) const {
            const auto i = cut_at(cuts, y);
            const unsigned page = page_of_content(i);
            const rect area = content_area(page);
            return placed{page, area.left() + x * points_per_px, area.top() + (y - cuts[i].top) * points_per_px};
        }

        [[nodiscard]] std::optional<placed> toc_place(const double x, const double y) const {
            if (toc_cuts.empty()) return std::nullopt;
            const auto i = cut_at(toc_cuts, y);
            const auto page = static_cast<unsigned>(toc_start + i);
            const rect area = content_area(page);
            return placed{page, area.left() + x * points_per_px, area.top() + (y - toc_cuts[i].top) * points_per_px};
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
            file->set_page(page_count());

            for (std::size_t i = 0; i < cuts.size(); ++i) {
                const unsigned page = page_of_content(i);
                file->set_page(page);
                draw_part(*file, recorded, cuts[i], content_area(page));
            }
            for (std::size_t i = 0; i < toc_cuts.size(); ++i) {
                const auto page = static_cast<unsigned>(toc_start + i);
                file->set_page(page);
                draw_part(*file, toc_recorded, toc_cuts[i], content_area(page));
            }
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
            if (!metadata.contains("Producer")) metadata["Producer"] = "simply-cpp html2pdf";
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
}
