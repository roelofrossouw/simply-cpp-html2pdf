# simply-cpp-html2pdf

HTML as pages of a PDF: the content flows from page to page under a header
and over a footer, with page numbers, a table of contents, links and
bookmarks. Laid out by [sc-litehtml](https://github.com/roelofrossouw/simply-cpp-litehtml)
(litehtml) and written by [sc-pdf](https://github.com/roelofrossouw/simply-cpp-pdf)
(on simply-cpp's own sc-pdflite), so it's **Apache-2.0** all the way down.

**Documentation:** the [simply-cpp wiki](https://github.com/roelofrossouw/simply-cpp/wiki) has an [HTML to PDF guide](https://github.com/roelofrossouw/simply-cpp/wiki/HTML-to-PDF) and the [sc-html2pdf reference](https://github.com/roelofrossouw/simply-cpp/wiki/Reference-sc-html2pdf).

## Usage

```cmake
find_package(sc-html2pdf CONFIG REQUIRED)

target_link_libraries(myapp PRIVATE sc::sc-html2pdf-shared)
```

```cpp
#include <html2pdf.h>

sc::html2pdf::options settings;
settings.header = "<p style='text-align: right'>#section# - page #Page# of #ToPage#</p>";
settings.footer = "<p style='font-size: 8pt'>Confidential</p>";
settings.css = "body { font-family: Roboto, sans-serif; font-size: 10pt }";
settings.margins = {.top = 10, .right = 10, .bottom = 10, .left = 30, .first_top = 60}; // mm
settings.toc_page = 2;                                    // a table of contents on page 2
settings.metadata["Author"] = "Accounts";
settings.protection = sc::pdf::protection{.user_password = "open"};

const sc::html2pdf pdf{html, settings};                   // laid out and cut into pages
pdf.save("report.pdf");
```

**Pages.** The content is laid out as wide as the margins leave, then cut
into pages between lines of text, images and boxes, never through them: a box
(background, border, an SVG's shapes) that fits on a page moves to the next
one whole with what's in it
(`split_boxes = true` cuts them as browsers print). `<section>`, or a style of
`page-break-before: always` (`break-before: page`), starts a new page;
`page-break-inside: avoid` keeps an element on one page.

**Built-in fonts.** `settings.builtin_fonts = true` sets the text in PDF's built-in Helvetica,
Times and Courier (CSS families map to them), which every reader has, so no font is embedded:
smaller files, a little faster. Another installed family named in the CSS stays itself, and
characters outside Windows-1252 come from an installed font, embedded.

**Headers and footers** are HTML laid out on every page with `#Page#`,
`#ToPage#` and `#section#`, `#subsection#` and `#subtitle#` (the text of the
last such element on or before the page; an image's file name becomes the
image) filled in. In a header, `#P1Head` shows on the first page only and
`#POHead` on the others.

**The table of contents** lists every `<section>` (its `alt`, else its text)
under the `<subtitle>`s before it, each linked to its page; the pages after it
are numbered on. The outline (bookmarks) holds the same entries, or else the
`h1` to `h3` headings.

**Links**: `#page=3` goes to page 3, `#id` to wherever that element landed,
anything else is resolved against the base URL.

## Demo

`sc-html2pdf-demo [OUTPUT]` writes a few pages with a header, footer, table of
contents and links:

<!-- sc-example: examples/sc-html2pdf-demo.cpp -->
```cpp
std::string body = "<h1>A report</h1><p>Written by sc::html2pdf from HTML and CSS.</p>";
for (const auto *part: {"Introduction", "Figures", "Conclusion"}) {
    body += std::string{"<section>"} + part + "</section><h2>" + part + "</h2>";
    for (int i = 0; i < 12; ++i)
        body += "<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et "
                "dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris.</p>";
    body += "<table style='width: 100%; border-collapse: collapse'><tr style='background: #e8f1f8'><th align='left'>Item</th>"
            "<th align='right'>Amount</th></tr><tr><td style='border-top: 1px solid #999'>Widgets</td>"
            "<td align='right' style='border-top: 1px solid #999'>120.00</td></tr></table>";
}
body += "<p>Back to <a href='#page=1'>the first page</a>, or see <a href='https://github.com/roelofrossouw/simply-cpp'>simply-cpp</a>.</p>";

sc::html2pdf::options settings;
settings.css = "body { font-family: sans-serif; font-size: 10pt } h1 { color: #2a6f97 }";
settings.header = "<p style='text-align: right; color: #666'>#section# &middot; page #Page# of #ToPage#</p>";
settings.footer = "<p style='font-size: 8pt; color: #999'>Made with simply-cpp</p>";
settings.margins.first_top = 20;
settings.toc_page = 2;
settings.metadata["Author"] = "sc-html2pdf-demo";
const sc::html2pdf pdf{body, settings};
SC_SHOW(pdf.page_count());
pdf.save(output);
```
<!-- /sc-example -->

The [html2pdf](https://github.com/roelofrossouw/html2pdf) program does the
same from the command line.

## Building locally

Run `bash scripts/install.sh` to configure, build, test, package and install.
It needs sc-litehtml and sc-pdf (installed if missing).

```bash
sudo curl -fsSL https://apt.roelof.co.za/setup.sh | bash
sudo apt -y install simply-cpp-html2pdf-dev
```
