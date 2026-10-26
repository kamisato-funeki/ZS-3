#include <placement/io/design.hpp>
#include <openssl/evp.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace placement {
namespace {
/** @brief 将摘要字节编码为小写十六进制。 @param bytes 摘要缓冲。 @param length 有效字节数。 @return 十六进制摘要。 */
std::string hexDigest(const unsigned char* bytes,unsigned int length) {
    std::ostringstream stream; // 与区域设置无关的十六进制编码流。
    for(unsigned int index=0;index<length;++index) // 当前摘要字节索引。
        stream<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<int>(bytes[index]);
    return stream.str();
}
} // namespace
/** @brief 内存摘要，详细契约见 design.hpp。 @param content 原始字节。 @return SHA-256。 */
std::string Sha256(const std::string& content) {
    std::array<unsigned char,EVP_MAX_MD_SIZE> digest{}; // OpenSSL输出缓冲。
    unsigned int length=0; // 实际摘要长度，字节。
    if(EVP_Digest(content.data(),content.size(),digest.data(),&length,EVP_sha256(),nullptr)!=1) Throw("IO_FAILURE","SHA-256失败");
    return hexDigest(digest.data(),length);
}
/** @brief 流式输入指纹，详细契约见 design.hpp。 @param path 文件。 @param control 取消。 @return 输入审计。 */
InputFile AuditFile(const std::string& path,ControlToken& control) {
    std::ifstream stream(path,std::ios::binary); // 输入二进制流，避免换行转换改变指纹。
    if(!stream) Throw("REFERENCE_MISSING","无法读取输入",path,true);
    std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free); // RAII摘要状态。
    if(!context || EVP_DigestInit_ex(context.get(),EVP_sha256(),nullptr)!=1) Throw("IO_FAILURE","无法初始化SHA-256",path);
    std::array<char,65536> buffer{}; // 固定64KiB读取缓冲，不积累原始数据。
    std::uint64_t size=0; // 已读取字节数。
    while(stream) {
        control.checkCancelled(); stream.read(buffer.data(),buffer.size());
        const std::streamsize amount=stream.gcount(); // 本次读取字节。
        if(amount>0 && EVP_DigestUpdate(context.get(),buffer.data(),static_cast<std::size_t>(amount))!=1) Throw("IO_FAILURE","摘要更新失败",path);
        size+=static_cast<std::uint64_t>(amount);
    }
    if(!stream.eof()) Throw("IO_FAILURE","输入读取失败",path);
    std::array<unsigned char,EVP_MAX_MD_SIZE> digest{}; // 最终摘要缓冲。
    unsigned int length=0; // 摘要长度。
    if(EVP_DigestFinal_ex(context.get(),digest.data(),&length)!=1) Throw("IO_FAILURE","摘要完成失败",path);
    return {std::filesystem::absolute(path).lexically_normal().string(),hexDigest(digest.data(),length),size};
}
/** @brief 加载成功后生成内容身份；详细契约见 design.hpp。 @param input 请求。 @param config 配置。 @param control 取消。 @return 只读数据库。 */
std::shared_ptr<const DesignDatabase> LoadDesign(const InputSpec& input,const Config& config,ControlToken& control) {
    validateConfig(config); control.checkCancelled();
    const bool bookshelf=!input.auxPath.empty(); // 输入格式分支。
    if(bookshelf==(!input.defPath.empty() || !input.lefPaths.empty())) Throw("INPUT_INVALID","请仅指定AUX或配套LEF+DEF");
    DesignDatabase db=bookshelf?LoadBookshelf(input.auxPath,config,control):LoadLefDef(input,control); // 未发布的事务数据库。
    std::string identity; // 内容及角色顺序决定的数据集指纹。
    for(const InputFile& file:db.inputFiles) identity+=file.sha256; // 当前输入文件摘要。
    db.datasetId=Sha256(identity);
    validateDatabase(db,false); control.checkCancelled();
    return std::make_shared<const DesignDatabase>(std::move(db));
}
} // namespace placement
