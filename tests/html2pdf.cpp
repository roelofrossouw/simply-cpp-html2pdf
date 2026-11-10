// sc::html2pdf: pages cut between lines, hard breaks, the table of contents' pages, and what the
// file holds (pages, links, bookmarks, information).
#include <html2pdf.h>
#include <pdflite/document.h>
#include <pdflite/reader.h>

#include "sc_test.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
    const auto directory = std::filesystem::temp_directory_path() / "sc-html2pdf-test";

    std::string read(const std::filesystem::path &path) {
        std::ifstream file{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{file}, {}};
    }

    bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

    // A page's content, its streams decoded one after another.
    std::string page_content(const sc::pdflite::reader &pdf, const std::size_t index) {
        using namespace sc::pdflite;
        const auto page = pdf.page(index);
        const auto *contents = page.find("Contents");
        if (!contents) return {};
        std::vector<object> parts;
        const bool one = contents->is<reference>() && pdf.is_stream(contents->as<reference>().id);
        if (const object resolved = one ? *contents : pdf.resolve(*contents); resolved.is<array>()) {
            for (const auto &part: resolved.as<array>()) parts.push_back(part);
        } else {
            parts.push_back(resolved);
        }
        std::string out;
        for (const auto &part: parts)
            if (part.is<reference>())
                if (const auto data = pdf.stream(part.as<reference>().id)) out += pdf.decoded(*data);
        return out;
    }

    std::size_t count(const std::string &text, const std::string &part) {
        std::size_t found = 0;
        for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++found;
        return found;
    }

    // Paragraphs enough for several pages.
    std::string paragraphs(const int many, const std::string &word = "Lorem") {
        std::string out;
        for (int i = 0; i < many; ++i) out += "<p>" + word + " " + std::to_string(i) + " ipsum dolor sit amet, consectetur adipiscing elit.</p>";
        return out;
    }
}

int main() {
    std::filesystem::create_directories(directory);

    SECTION("Long content flows onto more pages, cut between lines");
    {
        const sc::html2pdf pdf{paragraphs(120)};
        CHECK_LT(1u, pdf.page_count());
        const auto &cuts = pdf.cuts();
        CHECK_EQ(cuts.front().top, 0.0);
        for (std::size_t i = 1; i < cuts.size(); ++i) CHECK_EQ(cuts[i].top, cuts[i - 1].bottom);
        CHECK_EQ(sc::html2pdf{"<p>short</p>"}.page_count(), 1u);
    }

    SECTION("A section starts a new page; break styles too");
    {
        const sc::html2pdf pdf{"<p>one</p><section>Second</section><p>two</p><div style='page-break-before: always'>three</div>"};
        CHECK_EQ(pdf.page_count(), 3u);
        const sc::html2pdf first{"<section>First</section><p>at the top already</p>"};
        CHECK_EQ(first.page_count(), 1u); // no empty page before it
    }

    SECTION("A section right after a table: the table's last line stays with it");
    {
        // Each table ends where the next section starts; its last row's text reaches a little
        // past that (its descender), which mustn't push the row onto a page of its own. Each part
        // is well short of a page, in whichever font the system has for sans-serif.
        std::string html;
        for (int part = 0; part < 3; ++part) {
            html += "<section>Part " + std::to_string(part) + "</section><h2>Part " + std::to_string(part) + "</h2>";
            for (int i = 0; i < 8; ++i)
                html += "<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore "
                        "magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris.</p>";
            html += "<table style='width:100%;border-collapse:collapse'>";
            for (int row = 0; row < 10; ++row)
                html += "<tr><td style='border-top:1px solid #999'>Item " + std::to_string(row) + "</td><td align=right style='border-top:1px solid #999'>" +
                        std::to_string(row * 10) + ".00</td></tr>";
            html += "</table>";
        }
        sc::html2pdf::options settings;
        settings.margins.first_top.reset();
        const sc::html2pdf pdf{"<!DOCTYPE html><html><body style='font-family:sans-serif;font-size:10pt'>" + html + "</body></html>", settings};
        CHECK_EQ(pdf.page_count(), 3u); // a page a section
        for (const auto &page: pdf.cuts()) CHECK_LT(100.0, page.bottom - page.top); // no page with just the last row
    }

    SECTION("The first page's top margin leaves room for a letterhead");
    {
        sc::html2pdf::options tall, flat;
        flat.margins.first_top.reset();
        const auto many = paragraphs(60);
        const sc::html2pdf letterhead{many, tall}, plain{many, flat};
        CHECK_LT(letterhead.cuts().front().bottom, plain.cuts().front().bottom);
    }

    SECTION("A table of contents on page 2 pushes the content after it");
    {
        sc::html2pdf::options settings;
        settings.toc_page = 2;
        std::string html = "<subtitle>Part one</subtitle><section>Introduction</section>" + paragraphs(30) +
                           "<section alt='The middle'>Middle</section>" + paragraphs(30) + "<section>End</section><p>fin</p>";
        const sc::html2pdf pdf{html, settings};
        CHECK_EQ(pdf.page_count(), static_cast<unsigned>(pdf.cuts().size()) + 1);
        const auto path = directory / "toc.pdf";
        pdf.save(path);
        const auto file = read(path);
        CHECK(contains(file, "/Type /Outlines"));
        CHECK(contains(file, "/Title (Part one)"));
        CHECK(contains(file, "/Title (The middle)")); // its alt
        CHECK_LT(std::size_t{2}, count(file, "/Subtype /Link")); // the contents' entries
    }

    SECTION("Headers and footers with page numbers; links and information");
    {
        sc::html2pdf::options settings;
        settings.header = "<p id='P1Head'>First page</p><p id='POHead'>Page #Page# of #ToPage#</p>";
        settings.footer = "<p><a href='https://example.com'>example.com</a></p>";
        settings.metadata["Author"] = "html2pdf test";
        settings.protection.reset();
        const sc::html2pdf pdf{"<html><head><title>Report</title></head><body>" + paragraphs(80) + "<p id='end'>The end</p><p><a href='#end'>to the end</a></p></body></html>", settings};
        const auto path = directory / "headers.pdf";
        pdf.save(path);
        const auto file = read(path);
        CHECK(file.starts_with("%PDF-"));
        CHECK(contains(file, "/Count " + std::to_string(pdf.page_count())));
        CHECK(contains(file, "/URI (https://example.com)"));
        CHECK_EQ(count(file, "/URI (https://example.com)"), static_cast<std::size_t>(pdf.page_count())); // one per footer
        CHECK(contains(file, "/Title (Report)"));
        CHECK(contains(file, "/Author (html2pdf test)"));
        CHECK(contains(file, "/Producer (simply-cpp-html2pdf "));
        CHECK(contains(file, "simply-cpp-pdflite " + std::string{sc::pdflite::version()})); // in brackets after it
        CHECK(contains(file, "/Dest")); // #end
    }

    SECTION("Headers and footers laid out once for the pages that show the same: each page's own number");
    {
        // Laid out once for each section and length of page number, the number put in afterwards.
        // Digits in the text (2026, the section names) mustn't be taken for it.
        sc::html2pdf::options settings;
        settings.builtin_fonts = true; // text written as it is, to be found in the pages
        settings.header = "<table style='width: 100%'><tr><td>Version 2026-11 #section#</td><td style='text-align: right'>Page #Page#/#ToPage#</td></tr></table>";
        settings.footer = "<p style='text-align: center'>#Page# - 10 - 0 - #section#</p>";
        std::string html;
        for (int part = 1; part <= 3; ++part) html += "<section>Part " + std::to_string(part) + "</section>" + paragraphs(120);
        const sc::html2pdf pdf{html, settings};
        CHECK_LT(12u, pdf.page_count()); // one and two digits
        const auto path = directory / "numbered.pdf";
        pdf.save(path);
        const sc::pdflite::reader read_back{path};
        bool all = true;
        for (unsigned page = 1; page <= pdf.page_count(); ++page) {
            const auto text = page_content(read_back, page - 1);
            const auto number = std::to_string(page);
            const bool right = contains(text, "(" + number + "/" + std::to_string(pdf.page_count()) + ") Tj") && contains(text, "(" + number + ") Tj") &&
                               contains(text, "(2026-11) Tj");
            if (!right) CHECK_MSG(right, "page " + number + "'s header and footer");
            all = all && right;
        }
        CHECK(all);
    }

    SECTION("Images: a footer's logo written once for every page; a big PNG scaled down to 150 pixels an inch");
    {
        // banner.png is 600 by 200 pixels, shown at most 2 inches wide (192 px): 300 by 100 at 150 per inch.
        const std::string banner = SC_HTML2PDF_TEST_BANNER;
        sc::html2pdf::options settings;
        settings.footer = "<img src='" + banner + "' style='width: 96px'>";
        const sc::html2pdf pdf{"<img src='" + banner + "' style='width: 192px'>" + paragraphs(80), settings};
        CHECK_LT(2u, pdf.page_count());
        const auto path = directory / "images.pdf";
        pdf.save(path);
        auto file = read(path);
        CHECK_EQ(count(file, "/Subtype /Image"), std::size_t{1});
        CHECK(contains(file, "/Width 300 /Height 100 "));

        settings.image_resolution = 0; // kept as it is
        sc::html2pdf{"<img src='" + banner + "' style='width: 192px'>" + paragraphs(80), settings}.save(path);
        file = read(path);
        CHECK_EQ(count(file, "/Subtype /Image"), std::size_t{1});
        CHECK(contains(file, "/Width 600 /Height 200 "));
    }

    SECTION("A section whose text is an image's path: the image in #section#, its alt in the table of contents");
    {
        const std::string banner = SC_HTML2PDF_TEST_BANNER;
        sc::html2pdf::options settings;
        settings.header = "<table><tr><td style='width: 100px'>#section#</td></tr></table>";
        settings.toc_page = 1;
        const auto path = directory / "section-image.pdf";
        sc::html2pdf{"<section alt='Details of Brokerage'>\n  " + banner + "\n</section>" + paragraphs(20), settings}.save(path);
        auto file = read(path);
        CHECK_EQ(count(file, "/Subtype /Image"), std::size_t{1}); // the banner in the header, written once
        CHECK(contains(file, "Details of Brokerage"));             // the table of contents and outline
        CHECK(!contains(file, "banner.png"));                      // the path itself isn't text anywhere

        // A section of text shows its text, as before.
        sc::html2pdf{"<section alt='Cover'>Cover page</section>" + paragraphs(20), settings}.save(path);
        file = read(path);
        CHECK_EQ(count(file, "/Subtype /Image"), std::size_t{0});
    }

    SECTION("Built-in fonts: nothing embedded");
    {
        sc::html2pdf::options settings;
        settings.builtin_fonts = true;
        settings.header = "<p style='font-family: serif'>Page #Page#</p>";
        const auto path = directory / "builtin.pdf";
        sc::html2pdf{"<h1>Café</h1><p style='font-family: Arial'>Plain text, € and “quotes”.</p><pre>code</pre>", settings}.save(path);
        const auto file = read(path);
        CHECK(contains(file, "/BaseFont /Helvetica-Bold")); // the h1
        CHECK(contains(file, "/BaseFont /Times-Roman"));    // the header
        CHECK(contains(file, "/BaseFont /Courier"));        // pre
        CHECK(!contains(file, "/FontFile"));
    }

    SECTION("The PDF as bytes, as save() writes it");
    {
        const sc::html2pdf pdf{"<html><head><title>In memory</title></head><body>" + paragraphs(40) + "</body></html>"};
        const auto bytes = pdf.bytes();
        const auto path = directory / "bytes.pdf";
        pdf.save(path);
        CHECK(bytes.starts_with("%PDF-"));
        CHECK(contains(bytes, "/Title (In memory)"));
        CHECK_EQ(bytes.size(), read(path).size());
    }

    SECTION("Passwords");
    {
        sc::html2pdf::options settings;
        settings.protection = sc::pdf::protection{.user_password = "secret"};
        const auto path = directory / "locked.pdf";
        sc::html2pdf{"<p>hidden</p>", settings}.save(path);
        CHECK(contains(read(path), "/Encrypt"));
    }

    SECTION("A check called while laying out and writing; one that throws stops it, leaving no file");
    {
        int calls = 0;
        const sc::html2pdf pdf{paragraphs(200), {}, [&] { ++calls; }};
        const int laid_out = calls;
        CHECK(laid_out >= 5);
        const auto path = directory / "checked.pdf";
        pdf.save(path);
        CHECK(calls >= laid_out + static_cast<int>(pdf.page_count())); // at least once a page
        CHECK(std::filesystem::exists(path));

        // Stopped while laying out: the exception comes out as it is.
        int left = 3;
        CHECK_THROWS_AS(sc::html2pdf(paragraphs(200), {}, [&] { if (--left == 0) throw std::range_error{"stop"}; }), std::range_error);

        // Stopped while writing: no half-written file.
        bool stopping = false;
        const sc::html2pdf stopped{paragraphs(200), {}, [&] { if (stopping) throw std::range_error{"stop"}; }};
        stopping = true;
        CHECK_THROWS_AS(stopped.save(path), std::range_error);
        CHECK(!std::filesystem::exists(path));
        CHECK_THROWS_AS((void) stopped.bytes(), std::range_error);
    }

    SECTION("A border on one side of a cell is one band, with nothing drawn for the other sides");
    {
        // Was a ring whose inner edge lay on the outer one at the top, bottom and left: bands of no
        // width that Adobe Reader painted as hairlines, a box round every cell.
        sc::html2pdf::options settings;
        settings.builtin_fonts = true;
        const sc::html2pdf pdf{"<table style='border-collapse: collapse'><tr>"
                               "<td style='border-right: 1px solid #ff0000'>Caption</td><td>Value</td>"
                               "<td style='border: 2px solid #0000ff'>Boxed</td></tr></table>",
                               settings};
        const auto path = directory / "one-side.pdf";
        pdf.save(path);
        const auto text = page_content(sc::pdflite::reader{path}, 0);
        // Each fill in a colour: its path's subpaths (one m apiece) and points.
        const auto fills = [&](const std::string &colour) {
            std::vector<std::pair<std::size_t, std::size_t>> out; // subpaths, points
            for (auto at = text.find(colour); at != std::string::npos; at = text.find(colour, at + 1)) {
                const auto end = text.find("\nf", at);
                if (end == std::string::npos) break;
                const auto shape = text.substr(at, end - at);
                out.emplace_back(count(shape, " m"), count(shape, " m") + count(shape, " l"));
            }
            return out;
        };
        const auto red = fills("1 0 0 rg");
        CHECK_EQ(std::size_t{1}, red.size());
        if (!red.empty()) {
            CHECK_EQ(std::size_t{1}, red[0].first);  // one subpath: the right-hand band
            CHECK_EQ(std::size_t{4}, red[0].second); // a rectangle, no edges of no width beside it
        }
        const auto blue = fills("0 0 1 rg");
        CHECK_EQ(std::size_t{1}, blue.size());
        if (!blue.empty()) CHECK_EQ(std::size_t{2}, blue[0].first); // the box: still one ring, as before
    }

    SECTION("Mistakes");
    {
        sc::html2pdf::options settings;
        settings.margins.left = 150;
        settings.margins.right = 150;
        CHECK_THROWS_AS(sc::html2pdf("<p>x</p>", settings), std::invalid_argument);
        CHECK_THROWS_AS(sc::html2pdf{"<p>x</p>"}.save(directory / "no-such-directory" / "x.pdf"), std::runtime_error);
    }

    std::filesystem::remove_all(directory);
    TEST_SUMMARY();
}
