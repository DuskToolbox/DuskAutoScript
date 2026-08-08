#ifndef DAS_CORE_I18N_IDASI18NIMPL_H
#define DAS_CORE_I18N_IDASI18NIMPL_H

#include "Config.h"
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasI18n.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasMutableI18n.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasReadOnlyStringVector.Implements.hpp>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

DAS_CORE_I18N_NS_BEGIN

// IDasReadOnlyStringVector 的 i18n 内部构造工具：把 locale 名 / key 列表封装成
// IDasReadOnlyStringVector 出参。复用 autogen DasReadOnlyStringVectorImplBase
// CRTP 基类（引用计数 + QueryInterface），与 OrtWrapper 的同名 Impl 同源。
class DasI18nStringVectorImpl final
    : public Das::ExportInterface::DasReadOnlyStringVectorImplBase<
          DasI18nStringVectorImpl>
{
    std::vector<Das::DasPtr<IDasReadOnlyString>> items_;

public:
    DasI18nStringVectorImpl() = default;

    DAS_IMPL GetCount(uint32_t* p_count) override;
    DAS_IMPL GetAt(uint32_t index, IDasReadOnlyString** pp_out_value) override;

    void AddString(const std::string& str);
    void Reserve(size_t count);
};

// IDasI18n + IDasMutableI18n 实现：内部维护 locale -> (key -> text) 的双层
// map。 Translate 默认读进程级
// GlobalLocale（DasGetDefaultLocale），TranslateWithLocale 显式指定
// locale；locale 缺失 key 时回退到 base(en)，base 也缺失返回
// DAS_E_OUT_OF_RANGE。{N} 占位符原样透传，由消费方 vformat 填充。
// 单个 Impl 类继承 DasMutableI18nImplBase（派生接口基类），其 QueryInterface
// 同时处理 IDasI18n（父）与 IDasMutableI18n（自身），因此
// FromDirectory/FromFile 与 CreateIDasMutableI18n 三个工厂可共用同一实现。
class IDasI18nImpl final
    : public Das::ExportInterface::DasMutableI18nImplBase<IDasI18nImpl>
{
public:
    IDasI18nImpl() = default;

    // IDasI18n
    DAS_IMPL Translate(
        IDasReadOnlyString*  key,
        IDasReadOnlyString** pp_out_text) override;
    DAS_IMPL TranslateWithLocale(
        IDasReadOnlyString*  locale_name,
        IDasReadOnlyString*  key,
        IDasReadOnlyString** pp_out_text) override;
    DAS_IMPL GetAvailableLocales(
        Das::ExportInterface::IDasReadOnlyStringVector** pp_out_locales)
        override;
    DAS_IMPL GetKeys(
        IDasReadOnlyString*                              locale_name,
        Das::ExportInterface::IDasReadOnlyStringVector** pp_out_keys) override;

    // IDasMutableI18n
    DAS_IMPL AddTranslation(
        IDasReadOnlyString* locale_name,
        IDasReadOnlyString* key,
        IDasReadOnlyString* text) override;
    DAS_IMPL MergeFromFile(IDasReadOnlyString* json_path_utf8) override;

    // 工厂装载入口：扫描 {dir}/*.json，按文件名 stem 合并每个 locale。
    // 单个文件畸形返回 DAS_E_INVALID_JSON，已合并的 locale 保留（每个文件
    // 独立事务）。空目录返回 DAS_S_OK（产出空表，Translate 一律 miss）。
    DasResult LoadDirectory(const std::filesystem::path& dir);

private:
    using KeyToText = std::map<std::string, std::string>;
    std::map<std::string, KeyToText> translations_;
    std::string                      base_locale_{"en"};

    // 内部查表：先 locale，miss 且 locale != base 时回退 base。命中则填充
    // *pp_out_text（新建 IDasReadOnlyString 并 AddRef 移交）；miss 返回
    // DAS_E_OUT_OF_RANGE，*pp_out_text 不填充。
    DasResult Lookup(
        const std::string&   locale,
        const std::string&   key,
        IDasReadOnlyString** pp_out_text);

    // 把单个 per-locale 扁平 JSON 文件合并入 translations_[stem]。stem 决定
    // locale 名。畸形 JSON / 顶层非 object 返回 DAS_E_INVALID_JSON（事务性：
    // 先解析进临时 map，全成功再 swap，不半合并）。
    DasResult MergeSingleFile(const std::filesystem::path& json_path);
};

DAS_CORE_I18N_NS_END

#endif // DAS_CORE_I18N_IDASI18NIMPL_H
