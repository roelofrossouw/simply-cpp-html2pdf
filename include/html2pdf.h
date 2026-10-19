#pragma once

#include <html.h>
#include <page.h>
#include <pdf.h>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sc {
    /// HTML as a PDF of pages: the content flows from page to page under a header and over a
    /// footer, cut between lines of text, images and boxes, never through them. Lengths in the HTML are
    /// CSS pixels (a point is 4/3 of one; pt, mm and em work as in print).
    ///
    ///     sc::html2pdf::options settings;
    ///     settings.header = "<p style='text-align: right'>Page #Page# of #ToPage#</p>";
    ///     sc::html2pdf pdf{html, settings};
    ///     pdf.save("invoice.pdf");
    ///
    /// In the header and footer, #Page# is the page's number and #ToPage# the last page's; #section#,
    /// #subsection# and #subtitle# the text of the last such element on or before the page (an
    /// image's file name becomes the image). A header's #P1Head shows on the first page only and
    /// #POHead on the others.
    ///
    /// In the content, <section> starts a new page and an entry in the table of contents (its alt,
    /// else its text), <subtitle> a heading in it, <subsection> only sets #subsection#; all three are
    /// invisible. So does an element styled page-break-before: always (or break-before: page), and
    /// page-break-inside: avoid keeps an element on one page when it fits on one. A link to
    /// "#page=3" goes to page 3, to "#id" where that element is; others are left as they are,
    /// relative ones resolved against the base URL.
    class html2pdf {
    public:
        /// Lengths in millimetres.
        struct margins {
            double top = 10;
            double right = 10;
            double bottom = 10;
            double left = 30;
            /// The first page's top margin, when it differs (a letterhead's space, say).
            std::optional<double> first_top = 60;
        };

        struct options {
            page_size size = page_size::A4;
            orientation turned = orientation::portrait;
            html2pdf::margins margins;
            /// HTML above and below the content of every page (their #variables# filled in).
            std::string header;
            std::string footer;
            /// CSS for the content, header and footer, after the defaults and before their own.
            std::string css;
            /// The content's base URL, its fonts, @media type and fetching (sc::web::html_options;
            /// its css is replaced by the one above).
            web::html_options html;
            /// The page the table of contents starts on (pushing what was there after it); 0 for
            /// none. Its title, and the outline (bookmarks) made of its entries, or else of the
            /// h1 to h3 headings.
            unsigned toc_page = 0;
            std::string toc_title = "Table of Contents";
            bool outline = true;
            /// "Title", "Author", "Subject", "Keywords", "Creator", "Producer" or your own; the title
            /// defaults to the HTML's <title>.
            std::map<std::string, std::string> metadata;
            /// Passwords and rights; none leaves the file unencrypted.
            std::optional<pdf::protection> protection;
            /// Whether a box's background, border or image may be cut across two pages, as browsers
            /// print them. By default one that fits on a page moves to the next one whole, with
            /// what's in it.
            bool split_boxes = false;
            /// Outlines the margins, header and footer areas, to check a layout.
            bool draw_margins = false;
        };

        /// Where the content was cut: each page's top and bottom in the laid out content (px).
        struct cut {
            double top;
            double bottom;
        };

        /// Lays out the content (and header and footer) and cuts it into pages.
        explicit html2pdf(std::string_view html);

        html2pdf(std::string_view html, options settings);

        html2pdf(html2pdf &&) noexcept;
        html2pdf &operator=(html2pdf &&) noexcept;
        ~html2pdf();

        /// Pages in all, the table of contents' included.
        [[nodiscard]] unsigned page_count() const;

        /// The content's pages, in order (without the table of contents).
        [[nodiscard]] const std::vector<cut> &cuts() const;

        /// Writes the PDF; throws std::runtime_error when it can't.
        void save(const std::filesystem::path &file) const;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
}
