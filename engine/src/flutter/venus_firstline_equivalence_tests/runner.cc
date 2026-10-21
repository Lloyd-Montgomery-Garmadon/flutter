#include <algorithm>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "flutter/fml/icu_util.h"
#include "include/core/SkGraphics.h"
#include "include/core/SkFontMgr.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#include "tools/Resources.h"
#include "tests/Test.h"
#include "tools/flags/CommandLineFlags.h"
static DEFINE_string(icu_data, "", "Exact arm icudtl.dat");
bool OfficialFontsReady();
void DiagnosticZalgo(skiatest::Reporter* reporter);
void InlineEdgeTests(skiatest::Reporter* reporter);
namespace ToolUtils {
static DEFINE_bool(nativeFonts, true, "Use official FreeType test fonts");
sk_sp<SkFontMgr> TestFontMgr() {
  SkASSERT_RELEASE(FLAGS_nativeFonts);
  static auto manager = [] {
    FcConfig* config = FcConfigCreate();
    const auto path = GetResourcePath("fontconfig/fonts.conf");
    SkASSERT_RELEASE(FcConfigParseAndLoad(config, reinterpret_cast<const FcChar8*>(path.c_str()), true));
    SkASSERT_RELEASE(FcConfigBuildFonts(config));
    return SkFontMgr_New_FontConfig(config, SkFontScanner_Make_FreeType());
  }();
  return manager;
}
}
class Reporter final : public skiatest::Reporter {
 public:
  std::atomic<int> failures{0};
  void reportFailed(const skiatest::Failure& failure) override {
    ++failures;
    std::lock_guard<std::mutex> lock(mutex_);
    std::fprintf(stderr, "%s\n", failure.toString().c_str());
  }
 private:
  std::mutex mutex_;
};
int main(int argc, char** argv) {
  CommandLineFlags::Parse(argc, argv);
  if (FLAGS_icu_data.size() != 1) return 2;
  fml::icu::InitializeICU(FLAGS_icu_data[0]);
  SkGraphics::Init();
  if (!OfficialFontsReady()) return 3;
  if (std::getenv("FIRSTLINE_ZALGO_DIAGNOSTIC")) {
    Reporter reporter; DiagnosticZalgo(&reporter);
    return reporter.failures == 0 ? 0 : 1;
  }
  std::vector<const skiatest::Test*> tests;
  for (const auto& test : skiatest::TestRegistry::Range()) {
    if (std::string(test.fName).find("SkParagraph_") != 0) return 4;
    tests.push_back(&test);
  }
  std::sort(tests.begin(), tests.end(), [](auto* a, auto* b) {
    return std::string(a->fName) < std::string(b->fName);
  });
  if (tests.size() != 142) {
    std::fprintf(stderr,"Unexpected registration count=%zu\n",tests.size());
    return 5;
  }
  Reporter reporter;
  size_t completed = 0;
  for (auto* test : tests) {
    if (test->fTestType != skiatest::TestType::kCPU &&
        test->fTestType != skiatest::TestType::kCPUSerial) return 6;
    std::printf("RUN %s\n", test->fName);
    std::fflush(stdout);
    test->cpu(&reporter);
    ++completed;
    std::printf("DONE %s\n", test->fName);
  }
  const int before_inline_edges = reporter.failures.load();
  InlineEdgeTests(&reporter);
  std::printf("INLINE_EDGE_DONE additional_failures=%d\n",
              reporter.failures.load() - before_inline_edges);
  std::printf("registered=%zu completed=%zu failures=%d\n",
              tests.size(), completed, reporter.failures.load());
  return completed == 142 && reporter.failures == 0 ? 0 : 1;
}
