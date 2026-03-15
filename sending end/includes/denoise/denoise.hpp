/*
    该文件重写了之前的降噪模块，降噪算法保持不变
*/
#include <cstdint>

class denoise {
public:
    denoise() = default;
    // 该函数实现对输入数据的噪音处理，输入为数据缓冲区和长度
    void process(int16_t *buf, int len);

private:
    // 高于某值的变化被认为是异常的（单位：绝对差值）
    // 该函数可以限制增幅不超过 max_delta，从而平滑过渡，减少突变带来的听感不适
    static int16_t slew_limit(int16_t curr, int16_t prev, int32_t max_delta);

public:
private:
    // 跨帧保持前一样本（用于跨帧连续性判断）
    int16_t last_left = 0;
    int16_t last_right = 0;

    // 自适应阈值相关常量
    static constexpr int32_t thresh_ultra = 500;
    static constexpr int32_t thresh_strict = 1000;
    static constexpr int32_t thresh_normal = 2000;
    static constexpr int32_t thresh_relaxed = 4000;

    // "小变化"判定阈值（绝对差值 < 该值 → 认为是正常信号）
    static constexpr int32_t small_diff_thresh = 500;

    // 中间区间阈值（不修改原有值，仅在原有档位之间插入过渡档）
    static constexpr int32_t thresh_ultra_strict  = 750;  // 500 ~ 1000 之间
    static constexpr int32_t thresh_strict_normal = 1500; // 1000 ~ 2000 之间
    static constexpr int32_t thresh_normal_relaxed = 3000; // 2000 ~ 4000 之间

    // 占比阈值（千分比）
    static constexpr uint32_t ratio_ultra = 900;         // 90% → 超严格
    static constexpr uint32_t ratio_ultra_strict  = 825; // 82.5% → 超严格~严格过渡
    static constexpr uint32_t ratio_strict = 750;        // 75% → 严格
    static constexpr uint32_t ratio_strict_normal = 675; // 67.5% → 严格~普通过渡
    static constexpr uint32_t ratio_normal = 600;        // 60% → 普通
    static constexpr uint32_t ratio_normal_relaxed = 480; // 48% → 普通~宽松过渡
};