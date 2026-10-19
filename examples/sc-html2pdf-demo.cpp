// Writes a few pages of HTML as a PDF with sc::html2pdf: a header with page numbers, a footer, a
// table of contents on page 2, sections, a table and links. --help lists the options.

#include <filesystem>
#include <iostream>
#include <string>

#include <params.h>
#include <sc.h>

#include <html2pdf.h>

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-html2pdf-demo"},
        {"description", "Writes a few pages of HTML as a PDF with sc::html2pdf, with a header and footer, page numbers, a "
                        "table of contents and links, to show what a report comes out like. Open the PDF it writes to have a "
                        "look.\nMore on the wiki: https://github.com/roelofrossouw/simply-cpp/wiki/HTML-to-PDF"},
        {"positional", {
            {"output", {{"default", (std::filesystem::temp_directory_path() / "sc-html2pdf-demo.pdf").string()},
                        {"help", "the PDF to write"}}}}}}};

    try {
        sc::console::title("simply-cpp html2pdf: HTML as pages");
        const std::filesystem::path output = args.as<std::string>("output");

        // [readme]
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
        // [/readme]
        sc::console::note("saved " + output.string());
    } catch (const std::exception &error) {
        std::cerr << "sc-html2pdf-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
