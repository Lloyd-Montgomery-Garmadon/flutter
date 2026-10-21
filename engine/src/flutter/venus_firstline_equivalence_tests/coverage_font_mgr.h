// Test-only missing directory-fallback adapter, identical in both engines.
#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>
#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkStream.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
class CoverageFontMgr final : public SkFontMgr {
 public:
  CoverageFontMgr(sk_sp<SkFontMgr> p, std::vector<sk_sp<SkFontMgr>> extra): primary_(std::move(p)) {
    managers_.push_back(primary_);
    for (auto& m: extra) if(m) managers_.push_back(std::move(m));
    for (const auto& m: managers_) {
      std::vector<Family> families;
      for(int i=0;i<m->countFamilies();++i) {SkString n;m->getFamilyName(i,&n);families.push_back({n,m->createStyleSet(i)});}
      std::sort(families.begin(),families.end(),[](const auto& a,const auto& b){return std::string_view(a.name.c_str())<std::string_view(b.name.c_str());});
      families_.push_back(std::move(families));
    }
  }
 private:
  struct Family {SkString name;sk_sp<SkFontStyleSet> styles;};
  static sk_sp<SkTypeface> pick(SkFontStyleSet* set,const SkFontStyle& style,SkUnichar cp) {
    if(!set)return nullptr;
    auto f=set->matchStyle(style);if(f&&f->unicharToGlyph(cp))return f;
    for(int i=0;i<set->count();++i){f=set->createTypeface(i);if(f&&f->unicharToGlyph(cp))return f;}return nullptr;
  }
  sk_sp<SkTypeface> onMatchFamilyStyleCharacter(const char family[],const SkFontStyle& style,const char* langs[],int count,SkUnichar cp) const override {
    auto f=primary_->matchFamilyStyleCharacter(family,style,langs,count,cp);if(f&&f->unicharToGlyph(cp))return f;
    if(family) for(const auto& m:managers_){auto set=m->matchFamily(family);f=pick(set.get(),style,cp);if(f)return f;}
    for(const auto& fs:families_)for(const auto& candidate:fs){f=pick(candidate.styles.get(),style,cp);if(f)return f;}
    return nullptr;
  }
  int onCountFamilies()const override{return primary_->countFamilies();}
  void onGetFamilyName(int i,SkString* n)const override{primary_->getFamilyName(i,n);}
  sk_sp<SkFontStyleSet> onCreateStyleSet(int i)const override{return primary_->createStyleSet(i);}
  sk_sp<SkFontStyleSet> onMatchFamily(const char n[])const override{return primary_->matchFamily(n);}
  sk_sp<SkTypeface> onMatchFamilyStyle(const char n[],const SkFontStyle& s)const override{return primary_->matchFamilyStyle(n,s);}
  sk_sp<SkTypeface> onMakeFromData(sk_sp<SkData> d,int i)const override{return primary_->makeFromData(std::move(d),i);}
  sk_sp<SkTypeface> onMakeFromStreamIndex(std::unique_ptr<SkStreamAsset> s,int i)const override{return primary_->makeFromStream(std::move(s),i);}
  sk_sp<SkTypeface> onMakeFromStreamArgs(std::unique_ptr<SkStreamAsset> s,const SkFontArguments& a)const override{return primary_->makeFromStream(std::move(s),a);}
  sk_sp<SkTypeface> onMakeFromFile(const char n[],int i)const override{return primary_->makeFromFile(n,i);}
  sk_sp<SkTypeface> onLegacyMakeTypeface(const char n[],SkFontStyle s)const override{return primary_->legacyMakeTypeface(n,s);}
  sk_sp<SkFontMgr> primary_;
  std::vector<sk_sp<SkFontMgr>> managers_;
  std::vector<std::vector<Family>> families_;
};
