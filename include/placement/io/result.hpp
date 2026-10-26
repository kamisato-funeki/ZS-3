#pragma once
#include <placement/analysis/analysis.hpp>
#include <map>

namespace placement {
/** @brief 从JSON文本读取共享配置；所有未知字段报错。
 * @param text JSON对象，schemaVersion可省略或为1。 @param base 覆盖前配置。
 * @return 已验证有效配置；格式和范围错误抛INPUT_INVALID。 */
Config ParseConfig(const std::string& text,const Config& base=Config{});
/** @brief 将显式CLI字符串覆盖到共享配置，不修改base。
 * @param base 文件/默认配置。 @param values snake_case键及值文本。
 * @return 验证后的配置；未知键或非法数值抛INPUT_INVALID。 */
Config OverrideConfig(const Config& base,const std::map<std::string,std::string>& values);
/** @brief 配置完整序列化，无副作用。 @param config 有效配置。 @return schemaVersion=1 JSON。 */
std::string SerializeConfig(const Config& config);
/** @brief 生成输入审计JSON。 @param database 已加载数据库。 @return 含数量/单位/指纹/警告的JSON。 */
std::string SerializeAudit(const DesignDatabase& database);
/** @brief 生成快照JSON，禁止非有限坐标或固定漂移。
 * @param snapshot 完整几何。 @return 带身份、指标、真实实例的JSON，不含filler。 */
std::string SerializeSnapshot(const Snapshot& snapshot);
/** @brief 生成绑定源快照身份的违规JSON。 @param snapshot 来源。 @param report 独立检查结果。 @return JSON。 */
std::string SerializeViolations(const Snapshot& snapshot,const ViolationReport& report);
/** @brief 读取placement并核对原始输入指纹，不恢复优化器动量。
 * @param database 当前只读数据库。 @param path placement JSON。 @param control 取消令牌。
 * @return 绑定该数据库的完整快照；身份/固定性/版本不符拒绝。 */
Snapshot LoadSnapshot(std::shared_ptr<const DesignDatabase> database,const std::string& path,ControlToken& control);
/** @brief 读UTF-8小型JSON/配置文本。 @param path 文件路径。 @return 原文；不可读抛IO_FAILURE。 */
std::string ReadText(const std::string& path);
/** @brief 在同目录临时文件中执行写出并原子提交；默认不覆盖已存在结果。
 * @param path 目标路径。 @param writer 同步写出回调，输入临时路径，必须关闭文件后返回。
 * @param control 独立取消令牌。 @param overwrite 是否明确允许替换。
 * 无返回值；失败清理本次临时文件，保留原目标，抛Error。 */
void AtomicWrite(const std::string& path,const std::function<void(const std::string&)>& writer,ControlToken& control,bool overwrite=false);
/** @brief 原子保存文本。 @param path 目标。 @param text 完整内容。 @param control 取消。 @param overwrite 显式覆盖许可。 无返回值。 */
void WriteTextAtomic(const std::string& path,const std::string& text,ControlToken& control,bool overwrite=false);
} // namespace placement
