#include "wifi_types.hpp"
namespace sed_ws63 {

softapconfig::softapconfig()
{
    // SED : 构造函数中应该包含从不易失存储中读取配置的功能，当前先使用默认值
    
}

void softapconfig::set_ssid(const char *new_ssid)
{
    if (new_ssid == nullptr) {
        return;
    }
    // SED ： 应该还有存储功能，当前先不实现
}

void softapconfig::set_password(const char *new_password)
{
    if (new_password == nullptr) {
        return;
    }
    // SED ： 应该还有存储功能，当前先不实现
}

stacredential::stacredential()
{
    // SED : 构造函数中可以包含一些默认值或者初始化逻辑，当前先保持空实现
}

void stacredential::set_ssid(const char *new_ssid)
{
    if (new_ssid == nullptr) {
        return;
    }
    // SED ： 应该还有存储功能，当前先不实现
}

void stacredential::set_password(const char *new_password)
{
    if (new_password == nullptr) {
        return;
    }
    // SED ： 应该还有存储功能，当前先不实现
}

}