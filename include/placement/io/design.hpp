#pragma once
#include <placement/core/model.hpp>

namespace placement {
/** @brief 格式无关输入请求；AUX 与 LEF+DEF 两种来源互斥，由调用者拥有路径。 */
struct InputSpec {
    std::string auxPath; ///< BookShelf AUX 路径；LEF/DEF 模式为空。
    std::vector<std::string> lefPaths; ///< 配套 LEF 文件列表，按声明顺序合并。
    std::string defPath; ///< 单个 DEF 路径；BookShelf 模式为空。
};
/** @brief 事务式加载并验证只读数据库；不准备或隐式修正原始位置。
 * @param input 互斥的格式路径；关联文件相对 AUX 所在目录解析。
 * @param config 单位配置。 @param control 仅取消令牌，解析不暂停。
 * @return 新数据库，调用成功后方可替换旧项目；输入/引用/格式失败抛 Error。 */
std::shared_ptr<const DesignDatabase> LoadDesign(const InputSpec& input, const Config& config, ControlToken& control);
/** @brief 加载 BookShelf 关联文件，供 LoadDesign 使用。
 * @param path AUX 路径。 @param config 单位配置。 @param control 取消令牌。
 * @return 可独占构建的数据库；错误携带路径或对象上下文。 */
DesignDatabase LoadBookshelf(const std::string& path, const Config& config, ControlToken& control);
/** @brief 使用 Limbo 所附 LEF/DEF 5.8 解析器映射领域模型；内部串行化非重入解析器。
 * @param input LEF 列表及 DEF。 @param control 取消令牌。
 * @return 单位统一为 μm 的数据库；不支持约束或引用缺失抛 Error。 */
DesignDatabase LoadLefDef(const InputSpec& input, ControlToken& control);
/** @brief 读取文件并记录 SHA-256、字节数，流式处理可取消。
 * @param path 输入文件。 @param control 取消令牌。
 * @return 规范化输入审计；不可读抛 IO_FAILURE。 */
InputFile AuditFile(const std::string& path, ControlToken& control);
/** @brief 对 UTF-8/二进制内容求 SHA-256，无文件副作用。
 * @param content 原始字节串。 @return 64 位十六进制摘要。 */
std::string Sha256(const std::string& content);
} // namespace placement
