#pragma once
#include <stdint.h>
namespace sed_ws63 { class iic_master { public: iic_master(int,int,bool=true){} bool iic_master_read_only(uint8_t*, uint8_t, uint16_t){ return true; } bool iic_master_read(uint8_t*, uint8_t, uint8_t*, uint8_t, uint16_t){ return true; } }; }
