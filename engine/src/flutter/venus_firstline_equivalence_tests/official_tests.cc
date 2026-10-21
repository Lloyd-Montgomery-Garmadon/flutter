// Test-only: preserve upstream assertions; enable its Unix-tagged cases on mac.
#include "tests/Test.h"
#undef UNIX_ONLY_TEST
#define UNIX_ONLY_TEST DEF_TEST
#include "modules/skparagraph/tests/SkParagraphTest.cpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
static DEFINE_string(inline_edge_default_bits, "", "Optional default metrics and RGBA bitstream output");
bool OfficialFontsReady() {
  if (FLAGS_paragraph_fonts.size() != 1 ||
      !FLAGS_run_paragraph_tests_needing_system_fonts) return false;
  auto fonts = sk_make_sp<ResourceFontCollection>();
  return fonts->fontsFound();
}

// Independent observation; original registered case remains unchanged.
void DiagnosticZalgo(skiatest::Reporter* reporter) {
    sk_sp<ResourceFontCollection> fontCollection = sk_make_sp<ResourceFontCollection>();
    SKIP_IF_FONTS_NOT_FOUND(reporter, fontCollection)
    NEED_SYSTEM_FONTS(fontCollection)

    TestCanvas canvas("SkParagraph_MultiStyle_Zalgo.png");

    canvas.get()->drawColor(SK_ColorWHITE);

    //SkString text(">S͛ͭ̋͆̈̔̇͗̍͑̎ͪͮͧͣ̽ͫͣ́ͬ̀͌͑͂͗͒̍̔̄ͧ̏̉̌̊̊̿̀̌̃̄͐̓̓̚̚҉̵̡͜͟͝͠͏̸̵̡̧͜҉̷̡͇̜̘̻̺̘̟̝͙̬̘̩͇̭̼̥̖̤̦͎k͉̩̘͚̜̹̗̗͍̤̥̱͉̳͕͖̤̲̣͚̮̞̬̲͍͔̯̻̮̞̭͈̗̫͓̂ͨ̉ͪ̒͋͛̀̍͊ͧ̿̅͆̓̔̔ͬ̇̑̿ͩ͗ͮ̎͌̿̄ͅP̴̵̡̡̛̪͙̼̣̟̩̭̫̱͙̬͔͉͍̘̠͉̦̝̘̥̟̗͖̫̤͕̙̬̦͍̱̖̮̱͑͐̎̃̒͐͋̚͘͞a̶̶̵̵̵̶̶̡̧̢̢̺͔̣͖̭̺͍̤͚̱̜̰̥͕̬̥̲̞̥̘͇͚̺̰͚̪̺͔̤͍̓̿͆̎͋̓ͦ̈́ͦ̌́̄͗̌̓͌̕͜͜͟͢͝͡ŕ͎̝͕͉̻͎̤̭͚̗̳̖̙̘͚̫͖͓͚͉͔͈̟̰̟̬̗͓̟͚̱̕͡ͅͅͅa̸̶̢̛̛̽ͮͩ̅͒ͫ͗͂̎ͦ̈́̓̚͘͜͢͡҉̷̵̶̢̡̜̮̦̜̥̜̯̙͓͔̼̗̻͜͜ͅḡ̢̛͕̗͖̖̤̦̘͔ͨͨ̊͒ͩͭͤ̍̅̃ͪ̋̏̓̍̋͗̋ͨ̏̽̈́̔̀̋̉ͫ̅̂ͭͫ̏͒͋ͥ̚͜r̶̢̧̧̥̤̼̀̂̒ͪ͌̿͌̅͛ͨͪ͒̍ͥ̉ͤ̌̿̆́ͭ͆̃̒ͤ͛̊ͧ̽͘͝͠a̧̢̧̢͑͑̓͑ͮ̃͂̄͛́̈́͋̂͌̽̄͒̔́̇ͨͧͭ͐ͦ̋ͨ̍ͦ̍̋͆̔ͧ͑͋͌̈̓͛͛̚͢͜͜͏̴̢̧̛̳͍̹͚̰̹̻͔p̨̡͆ͦͣ͊̽̔͂̉ͣ̔ͣ̌̌̉̃̋̂͒ͫ̄̎̐͗̉̌̃̽̽́̀̚͘͜͟҉̱͉h̭̮̘̗͔̜̯͔͈̯̺͔̗̣̭͚̱̰̙̼̹͚̣̻̥̲̮͍̤͜͝<");
    std::u16string text16 = u">S\u035B\u036D\u030B\u0346\u0308\u0314\u0307\u0357\u030D\u0351\u030E\u036A\u036E\u0367\u0363\u033D\u036B\u0363\u0301\u036C\u0300\u034C\u0351\u0342\u0357\u0352\u030D\u0314\u0304\u0367\u030F\u031A\u0309\u030C\u030A\u030A\u033F\u0300\u030C\u0303\u0304\u0350\u0313\u031A\u0313\u0363\u0321\u035C\u035D\u035F\u0360\u0335\u034F\u0321\u0327\u0338\u035C\u0335\u0363\u0337\u0321\u0347\u031C\u0318\u033B\u033A\u0318\u031F\u031D\u0359\u032C\u0318\u0329\u0347\u032D\u033C\u0325\u0316\u0324\u0326\u034Ek\u0302\u0368\u0309\u036A\u0312\u034B\u035B\u0300\u030D\u034A\u0367\u033F\u0305\u0346\u0313\u0314\u0314\u036C\u0307\u0311\u033F\u0369\u0357\u036E\u030E\u034C\u033F\u0304\u0349\u0329\u0318\u035A\u031C\u0339\u0317\u0317\u034D\u0324\u0325\u0331\u0349\u0333\u0355\u0345\u0356\u0324\u0332\u0323\u035A\u032E\u031E\u032C\u0332\u034D\u0354\u032F\u033B\u032E\u031E\u032D\u0348\u0317\u032B\u0353P\u031A\u0351\u0350\u030E\u0303\u0312\u0350\u034B\u0334\u031B\u035E\u0358\u0321\u0335\u0321\u032A\u0359\u033C\u0323\u031F\u0329\u032D\u032B\u0331\u0359\u032C\u0354\u0349\u034D\u0318\u0320\u0349\u0326\u031D\u0318\u0325\u031F\u0317\u0356\u032B\u0324\u0355\u0319\u032C\u0326\u034D\u0331\u0316\u032E\u0331a\u0313\u033F\u0346\u030E\u034B\u0313\u0366\u0344\u0366\u030C\u0301\u0304\u0357\u030C\u0313\u034C\u035C\u0336\u035C\u0321\u0336\u035D\u0315\u0335\u0335\u0335\u035F\u0336\u0336\u0327\u0322\u0361\u0362\u0322\u033A\u0354\u0323\u0356\u032D\u033A\u034D\u0324\u035A\u0331\u031C\u0330\u0325\u0355\u032C\u0325\u0332\u031E\u0325\u0318\u0347\u035A\u033A\u0330\u035A\u032A\u033A\u0354\u0324\u034Dr\u0301\u0361\u0315\u034E\u031D\u0355\u0349\u033B\u034E\u0324\u0345\u0345\u032D\u035A\u0317\u0333\u0316\u0319\u0318\u035A\u0345\u032B\u0356\u0353\u035A\u0349\u0354\u0348\u031F\u0330\u031F\u032C\u0317\u0353\u031F\u035A\u0331a\u033D\u036E\u0369\u0305\u0352\u031A\u036B\u0357\u0342\u030E\u0366\u0344\u0343\u0338\u035C\u0361\u0322\u031B\u0358\u031B\u0362\u0336\u0363\u0337\u035C\u0322\u035C\u0321\u0335\u0336\u0345\u031C\u032E\u0326\u031C\u0325\u031C\u032F\u0319\u0353\u0354\u033C\u0317\u033Bg\u0304\u0368\u0368\u030A\u0352\u0369\u036D\u0364\u030D\u0305\u0303\u036A\u030B\u030F\u0313\u030D\u031A\u030B\u0357\u030B\u0368\u030F\u033D\u0344\u0314\u0300\u030B\u0309\u036B\u0305\u0302\u036D\u036B\u030F\u0352\u034B\u0365\u0322\u031B\u035C\u0355\u0317\u0356\u0316\u0324\u0326\u0318\u0354r\u0300\u0302\u0312\u036A\u034C\u033F\u034C\u0305\u035B\u0368\u036A\u0352\u030D\u0365\u0309\u0364\u030C\u033F\u0306\u0301\u036D\u0346\u0303\u0312\u0364\u035B\u030A\u0367\u033D\u035D\u0360\u0322\u0358\u0327\u0327\u0336\u0325\u0324\u033Ca\u0351\u0351\u0313\u0351\u036E\u0303\u0342\u0304\u035B\u0301\u0344\u034B\u0302\u034C\u033D\u0304\u0352\u0314\u0301\u0307\u0368\u0367\u036D\u0350\u0366\u031A\u030B\u0368\u030D\u0366\u030D\u030B\u0346\u0314\u0367\u0351\u034B\u034C\u0308\u0343\u035B\u035B\u0327\u0322\u0327\u0362\u035C\u035C\u0322\u034F\u0322\u031B\u0334\u0327\u0333\u034D\u0339\u035A\u0330\u0339\u033B\u0354p\u0346\u0366\u031A\u0363\u034A\u033D\u0314\u0342\u0309\u0363\u0314\u0363\u030C\u030C\u0309\u0303\u030B\u0302\u0352\u036B\u0304\u030E\u0310\u0357\u0309\u030C\u0303\u033D\u033D\u0328\u0341\u0358\u0340\u0321\u035C\u035F\u0363\u0331\u0349h\u035C\u035D\u032D\u032E\u0318\u0317\u0354\u031C\u032F\u0354\u0348\u032F\u033A\u0354\u0317\u0323\u032D\u035A\u0331\u0330\u0319\u033C\u0339\u035A\u0323\u033B\u0325\u0332\u032E\u034D\u0324<";
    SkString text = SkUnicode::convertUtf16ToUtf8(text16);
    auto K = text.find("k");
    auto P = text.find("P");
    auto h = text.find("h");
    ParagraphStyle paragraph_style;
    ParagraphBuilderImpl builder(paragraph_style, fontCollection, get_unicode());
    TextStyle text_style;
    text_style.setFontFamilies({SkString("Roboto")});
    text_style.setFontSize(20);
    text_style.setColor(SK_ColorRED);
    builder.pushStyle(text_style);
    builder.addText(text.data(), K + 3);
    text_style.setColor(SK_ColorBLUE);
    text_style.setBackgroundColor(SkPaint(SkColors::kYellow));
    builder.pushStyle(text_style);
    builder.addText(text.data() + K + 3, P - K - 3 + 6);
    text_style.setColor(SK_ColorGREEN);
    builder.pushStyle(text_style);
    builder.addText(text.data() + P + 6, h - P - 6);
    text_style.setColor(SK_ColorBLACK);
    text_style.setBackgroundColor(SkPaint(SkColors::kLtGray));
    builder.pushStyle(text_style);
    builder.addText(text.data() + h, text.size() - h);
    auto paragraph = builder.Build();
    paragraph->layout(TestCanvasWidth);
    for (const auto& run : static_cast<ParagraphImpl*>(paragraph.get())->runs()) {
      SkString family, postscript;
      run.font().getTypeface()->getFamilyName(&family);
      run.font().getTypeface()->getPostScriptName(&postscript);
      std::printf("DIAGNOSTIC_ZALGO_FONT %zu %zu %s %s %.9f\n", run.textRange().start, run.textRange().end, family.c_str(), postscript.c_str(), run.font().getSize());
    }
    paragraph->paint(canvas.get(), 0, paragraph->getHeight() / 2);
    //auto width = paragraph->getLongestLine();
    auto height = paragraph->getHeight();

    auto resSK = paragraph->getRectsForRange(0, K, RectHeightStyle::kTight,
                                          RectWidthStyle::kTight);
    REPORTER_ASSERT(reporter, resSK.size() != 0);
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(resSK[0].rect.width(), 10.45f, EPSILON100));

    auto resKP = paragraph->getRectsForRange(K, P, RectHeightStyle::kTight,
                                          RectWidthStyle::kTight);
    REPORTER_ASSERT(reporter, resKP.size() != 0);
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(resKP[0].rect.width(), 11.22f));
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(resKP[0].rect.width(), 11.22f, EPSILON100));

    auto resPh = paragraph->getRectsForRange(P, h, RectHeightStyle::kTight,
                                          RectWidthStyle::kTight);
    REPORTER_ASSERT(reporter, resPh.size() != 0);
    std::printf("DIAGNOSTIC_ZALGO_WIDTH actual=%.9f expected=%.9f tolerance=%.9f\n",resPh[0].rect.width(),67.26f,EPSILON20);
    REPORTER_ASSERT(reporter, SkScalarNearlyEqual(resPh[0].rect.width(), 67.26f, EPSILON20));

    auto posK = paragraph->getGlyphPositionAtCoordinate(resSK.back().rect.fRight, height/2);
    auto posP = paragraph->getGlyphPositionAtCoordinate(resKP.back().rect.fRight, height/2);
    auto posH = paragraph->getGlyphPositionAtCoordinate(resPh.back().rect.fRight, height/2);
    REPORTER_ASSERT(reporter, posK.position == 148 && posP.position == 264 && posH.position == 572);
}


// No new registry entries: all 142 upstream tests still execute unchanged.
namespace {
std::unique_ptr<Paragraph> EdgeParagraph(
    const sk_sp<ResourceFontCollection>& fonts, const std::u16string& text,
    int edge_mode, float width, size_t max_lines = SIZE_MAX, bool nowrap = false,
    const char* family = "Ahem", bool css_wrap = true, float edge_width = 4) {
    ParagraphStyle style;
    style.setVenusWordBoundaryWrap(css_wrap);
    style.setMaxLines(max_lines);
    ParagraphBuilderImpl builder(style, fonts, get_unicode());
    TextStyle glyphs;
    glyphs.setFontFamilies({SkString(family)});
    glyphs.setFontSize(20);
    glyphs.setColor(SK_ColorBLACK);
    glyphs.setVenusNoSoftWrap(nowrap);
    builder.pushStyle(glyphs);
    for (char16_t ch : text) {
        if (ch == u'^' || ch == u'$' || ch == u'@') {
            PlaceholderStyle edge(ch == u'@' ? 50 : edge_width, ch == u'@' ? 20 : 0,
                                  PlaceholderAlignment::kBaseline,
                                  TextBaseline::kAlphabetic, ch == u'@' ? 16 : 0);
#if defined(VENUS_INLINE_EDGE_TESTS)
            if (edge_mode >= 0 && ch != u'@') {
                edge.fVenusInlineEdge = edge_mode != 0;
                edge.fVenusInlineEdgeStart = edge_mode != 0 && ch == u'^';
            }
#else
            (void)edge_mode;
#endif
            builder.addPlaceholder(edge);
        } else {
            builder.addText(std::u16string(1, ch));
        }
    }
    auto paragraph = builder.Build();
    paragraph->layout(width);
    return paragraph;
}

std::vector<uint64_t> EdgeSnapshot(Paragraph* paragraph) {
    std::vector<uint64_t> result;
    auto scalar = [&](double value) {
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        result.push_back(bits);
    };
    scalar(paragraph->getMaxWidth());
    scalar(paragraph->getHeight());
    scalar(paragraph->getMinIntrinsicWidth());
    scalar(paragraph->getMaxIntrinsicWidth());
    scalar(paragraph->getAlphabeticBaseline());
    scalar(paragraph->getIdeographicBaseline());
    scalar(paragraph->getLongestLine());
    result.push_back(paragraph->didExceedMaxLines());
    std::vector<LineMetrics> lines;
    paragraph->getLineMetrics(lines);
    result.push_back(lines.size());
    for (const auto& line : lines) {
        result.insert(result.end(), {line.fStartIndex, line.fEndIndex,
            line.fEndExcludingWhitespaces, line.fEndIncludingNewline,
            uint64_t(line.fHardBreak), line.fLineNumber});
        for (double value : {line.fAscent, line.fDescent, line.fUnscaledAscent,
                             line.fHeight, line.fWidth, line.fLeft, line.fTop,
                             line.fBaseline}) scalar(value);
    }
    const auto boxes = paragraph->getRectsForPlaceholders();
    result.push_back(boxes.size());
    for (const auto& box : boxes) {
        for (double value : {double(box.rect.left()), double(box.rect.top()),
                             double(box.rect.right()), double(box.rect.bottom())}) scalar(value);
        result.push_back(uint64_t(box.direction));
    }
    SkBitmap pixels;
    pixels.allocN32Pixels(256, 256);
    SkCanvas canvas(pixels);
    canvas.clear(SK_ColorWHITE);
    paragraph->paint(&canvas, 0, 0);
    uint64_t hash = 14695981039346656037ull;
    const auto* bytes = static_cast<const uint8_t*>(pixels.getPixels());
    for (size_t i = 0; i < pixels.computeByteSize(); ++i) {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    const auto* rgba = static_cast<const uint32_t*>(pixels.getPixels());
    for (size_t i = 0; i < pixels.computeByteSize() / sizeof(uint32_t); ++i) {
        result.push_back(rgba[i]);
    }
    result.push_back(hash);
    return result;
}
}  // namespace

void InlineEdgeTests(skiatest::Reporter* reporter) {
    auto fonts = sk_make_sp<ResourceFontCollection>();
    SKIP_IF_FONTS_NOT_FOUND(reporter, fonts)
    // Explicit false must preserve every observable, including upstream's
    // independent placeholder words. This fixture also compiles with old headers.
    size_t default_cases = 0;
    uint64_t digest = 14695981039346656037ull;
    REPORTER_ASSERT(reporter, FLAGS_inline_edge_default_bits.size() <= 1);
    std::FILE* bits = FLAGS_inline_edge_default_bits.size() == 1
        ? std::fopen(FLAGS_inline_edge_default_bits[0], "wb") : nullptr;
    REPORTER_ASSERT(reporter, FLAGS_inline_edge_default_bits.size() == 0 || bits);
    for (const auto& text : {u"a^b$c", u"a ^b$ c", u"a^\nb$c", u"^^ab$$", u"a^@$b"}) {
        for (float width : {0.f, 23.f, 44.f, 64.f, 200.f}) {
            for (size_t max_lines : {SIZE_MAX, size_t(1)}) {
                for (bool css_wrap : {false, true}) {
                    auto unset = EdgeParagraph(fonts, text, -1, width, max_lines,
                                               false, "Ahem", css_wrap);
                    auto disabled = EdgeParagraph(fonts, text, 0, width, max_lines,
                                                  false, "Ahem", css_wrap);
                    const auto expected = EdgeSnapshot(unset.get());
                    REPORTER_ASSERT(reporter, expected == EdgeSnapshot(disabled.get()));
                    if (bits) {
                        const uint64_t count = expected.size();
                        REPORTER_ASSERT(reporter, std::fwrite(&count, sizeof(count), 1, bits) == 1);
                        REPORTER_ASSERT(reporter, std::fwrite(expected.data(), sizeof(uint64_t),
                                                             expected.size(), bits) == expected.size());
                    }
                    for (uint64_t item : expected) digest = (digest ^ item) * 1099511628211ull;
                    ++default_cases;
                }
            }
        }
    }
    if (bits) REPORTER_ASSERT(reporter, std::fclose(bits) == 0);
    auto legacy = EdgeParagraph(fonts, u"a^b$c", -1, 44);
    std::vector<LineMetrics> legacy_lines;
    legacy->getLineMetrics(legacy_lines);
    REPORTER_ASSERT(reporter, legacy_lines.size() == 2 && legacy_lines[0].fWidth == 44);
    REPORTER_ASSERT(reporter, legacy->getLongestLine() == 43.999969482421875);
    REPORTER_ASSERT(reporter, legacy->getMinIntrinsicWidth() == 20);
    REPORTER_ASSERT(reporter, legacy->getMaxIntrinsicWidth() == 68);
    std::printf("INLINE_EDGE_DEFAULT cases=%zu digest=%016llx\n", default_cases,
                static_cast<unsigned long long>(digest));
#if defined(VENUS_INLINE_EDGE_TESTS)
    PlaceholderStyle normal;
    REPORTER_ASSERT(reporter, !normal.fVenusInlineEdge && !normal.fVenusInlineEdgeStart);
    auto marked = normal;
    marked.fVenusInlineEdge = true;
    REPORTER_ASSERT(reporter, !normal.equals(marked));
    auto start = marked;
    start.fVenusInlineEdgeStart = true;
    REPORTER_ASSERT(reporter, !start.equals(marked));
    struct Case { const char16_t* text; float width; size_t lines; float min; float max; bool nowrap; float edge_width = 4; };
    const Case cases[] = {
        {u"a^b$c", 44, 1, 68, 68, false},
        {u"a^b$c", 44, 1, 60, 60, false, 0},
        {u"a$ ^b", 44, 2, 24, 68, false},
        {u"a^\u00a0b$c", 44, 1, 88, 88, false},
        {u"a ^b$c", 64, 2, 48, 88, false},
        {u"a^b$ c", 64, 2, 48, 88, false},
        {u"a-^b$c", 64, 2, 48, 88, false},
        {u"^^ab$$", 44, 1, 56, 56, false},
        {u"^a^b$c$", 44, 1, 76, 76, false},
        {u"a^\nb$c", 44, 2, 44, 44, false},
        {u"a ^b c$", 44, 1, 108, 108, true},
        {u"a^@$b", 44, 3, 58, 98, false},
    };
    for (size_t i = 0; i < std::size(cases); ++i) {
        const auto& c = cases[i];
        std::printf("INLINE_EDGE_TRUE case=%zu\n", i);
        auto paragraph = EdgeParagraph(fonts, c.text, 1, c.width, SIZE_MAX,
                                       c.nowrap, "Ahem", true, c.edge_width);
        REPORTER_ASSERT(reporter, paragraph->lineNumber() == c.lines);
        REPORTER_ASSERT(reporter, paragraph->getMinIntrinsicWidth() == c.min);
        REPORTER_ASSERT(reporter, paragraph->getMaxIntrinsicWidth() == c.max);
        auto clipped = EdgeParagraph(fonts, c.text, 1, c.width, 1,
                                     c.nowrap, "Ahem", true, c.edge_width);
        // Flutter folds min intrinsic into max intrinsic for maxLines == 1.
        REPORTER_ASSERT(reporter, clipped->getMinIntrinsicWidth() == c.max);
        REPORTER_ASSERT(reporter, clipped->getMaxIntrinsicWidth() == c.max);
        REPORTER_ASSERT(reporter, clipped->lineNumber() == 1);
        REPORTER_ASSERT(reporter, clipped->didExceedMaxLines() == (c.lines > 1));
    }
    auto nowrap_tail = EdgeParagraph(fonts, u"a\nb\n^c d$", 1, 44, 2, true);
    REPORTER_ASSERT(reporter, nowrap_tail->lineNumber() == 2);
    REPORTER_ASSERT(reporter, nowrap_tail->getMinIntrinsicWidth() == 68);
    REPORTER_ASSERT(reporter, nowrap_tail->getMaxIntrinsicWidth() == 68);
    // The same CJK break puts a start edge on the next line and an end edge on
    // the previous line. Derive the width from the selected font, not a guess.
    auto cjk = EdgeParagraph(fonts, u"中", 1, 200, SIZE_MAX, false, "Noto Sans CJK SC");
    auto cjk_second = EdgeParagraph(fonts, u"文", 1, 200, SIZE_MAX, false, "Noto Sans CJK SC");
    const float cjk_width = cjk->getMaxIntrinsicWidth() + 5;
    for (bool is_start : {false, true}) {
        auto paragraph = EdgeParagraph(fonts, is_start ? u"中^文" : u"中$文",
                                       1, cjk_width, SIZE_MAX, false, "Noto Sans CJK SC");
        std::vector<LineMetrics> lines;
        paragraph->getLineMetrics(lines);
        const auto boxes = paragraph->getRectsForPlaceholders();
        REPORTER_ASSERT(reporter, lines.size() == 2 && boxes.size() == 1);
        const float expected_min = std::max(
            cjk->getMaxIntrinsicWidth() + (is_start ? 0 : 4),
            cjk_second->getMaxIntrinsicWidth() + (is_start ? 4 : 0));
        REPORTER_ASSERT(reporter, SkScalarNearlyEqual(paragraph->getMinIntrinsicWidth(), expected_min));
        auto clipped = EdgeParagraph(fonts, is_start ? u"中^文" : u"中$文",
                                      1, cjk_width, 1, false, "Noto Sans CJK SC");
        REPORTER_ASSERT(reporter, clipped->getMinIntrinsicWidth() == paragraph->getMaxIntrinsicWidth());
        REPORTER_ASSERT(reporter, clipped->getMaxIntrinsicWidth() == paragraph->getMaxIntrinsicWidth());
        if (lines.size() == 2 && boxes.size() == 1) {
            REPORTER_ASSERT(reporter, boxes[0].rect.top() == lines[is_start ? 1 : 0].fBaseline);
            REPORTER_ASSERT(reporter, boxes[0].rect.width() == 4);
        }
        // Cache hits and narrower/wider relayout preserve the same boundary.
        const auto first = EdgeSnapshot(paragraph.get());
        paragraph->layout(200);
        paragraph->layout(cjk_width);
        REPORTER_ASSERT(reporter, first == EdgeSnapshot(paragraph.get()));
    }
    std::printf("INLINE_EDGE_TRUE cases=%zu cjk=2\n", std::size(cases));
#endif
}
