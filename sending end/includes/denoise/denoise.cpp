#include "denoise.hpp"

int16_t denoise::slew_limit(int16_t curr, int16_t prev, int32_t max_delta)
{
    int32_t diff = static_cast<int32_t>(curr) - static_cast<int32_t>(prev);
    if (diff > max_delta) {
        return static_cast<int16_t>(static_cast<int32_t>(prev) + max_delta);
    } else if (diff < -max_delta) {
        return static_cast<int16_t>(static_cast<int32_t>(prev) - max_delta);
    } else {
        return curr;
    }
}

void denoise::process(int16_t *buf, int len)
{
    // 确保输入长度为偶数（每两个样本为一帧：左声道和右声道）
    if (len % 2 != 0) {
        return;
    }

    uint32_t small_diff_count = 0;
    // 这两个变量用于记录前一个数值
    int16_t prev_left = last_left;
    int16_t prev_right = last_right;

    // 第一次循环：计算每个样本的差值，并统计小变化的数量和最大差值
    for (int i = 0; i < len; i += 2) {
        int32_t diff_left = (int32_t)buf[i] - (int32_t)prev_left;
        int32_t diff_right = (int32_t)buf[i + 1] - (int32_t)prev_right;

        int32_t abs_diff_left = diff_left < 0 ? -diff_left : diff_left;
        int32_t abs_diff_right = diff_right < 0 ? -diff_right : diff_right;

        if (abs_diff_left < small_diff_thresh)
            small_diff_count++;
        if (abs_diff_right < small_diff_thresh)
            small_diff_count++;

        prev_left = buf[i];
        prev_right = buf[i + 1];
    }

    // 计算小变化的占比（千分比）
    uint32_t small_ratio = (small_diff_count * 1000) / len;

    // 根据占比选择降噪级别（7 档，原有 4 个锚点不变，插入 3 个过渡档）
    int32_t threshold;
    if (small_ratio >= ratio_ultra) {
        threshold = thresh_ultra; // >=90%：500，声音平稳，极严格
    } else if (small_ratio >= ratio_ultra_strict) {
        threshold = thresh_ultra_strict; // >=82.5%：750，轻微活跃
    } else if (small_ratio >= ratio_strict) {
        threshold = thresh_strict; // >=75%：1000，中等活跃
    } else if (small_ratio >= ratio_strict_normal) {
        threshold = thresh_strict_normal; // >=67.5%：1500，较活跃
    } else if (small_ratio >= ratio_normal) {
        threshold = thresh_normal; // >=60%：2000，活跃
    } else if (small_ratio >= ratio_normal_relaxed) {
        threshold = thresh_normal_relaxed; // >=48%：3000，高动态过渡
    } else {
        threshold = thresh_relaxed; // <48%：4000，音乐高潮/强攻击
    }

    // 应用降噪
    for (int i = 0; i < len; i += 2) {
        int16_t curr_left = buf[i];
        int16_t curr_right = buf[i + 1];

        int32_t diff_l = (int32_t)curr_left - (int32_t)last_left;
        int32_t abs_diff_l = (diff_l < 0) ? -diff_l : diff_l;
        if (abs_diff_l > threshold) {
            // 去噪原则是：如果异常的下一位也是异常，则直接削平，否则取平均值
            int16_t next_left = (i + 2 < len) ? buf[i + 2] : curr_left;
            int32_t next_diff = (int32_t)next_left - (int32_t)curr_left;
            int32_t abs_next_diff = (next_diff < 0) ? -next_diff : next_diff;
            // 如果下一位的变化也超过阈值，并且方向相反，则认为当前值是异常的，取平均值，否则认为当前值是异常的，取上一个值
            if (abs_next_diff > threshold && next_diff * diff_l < 0) {
                buf[i] = (last_left + next_left) / 2;
            } else { // 否则认为当前值是异常的，直接限制幅度
                buf[i] = slew_limit(curr_left, last_left, threshold / 2);
                // buf[i] = last_left;
            }
        }

        int32_t diff_r = (int32_t)curr_right - (int32_t)last_right;
        int32_t abs_diff_r = (diff_r < 0) ? -diff_r : diff_r;
        if (abs_diff_r > threshold) {
            int16_t next_right = (i + 3 < len) ? buf[i + 3] : curr_right;
            int32_t next_diff = (int32_t)next_right - (int32_t)curr_right;
            int32_t abs_next_diff = (next_diff < 0) ? -next_diff : next_diff;
            if (abs_next_diff > threshold && next_diff * diff_r < 0) {
                buf[i + 1] = (last_right + next_right) / 2;
            } else {
                buf[i + 1] = slew_limit(curr_right, last_right, threshold / 2);
                // buf[i + 1] = last_right;
            }
        }

        last_left = buf[i];
        last_right = buf[i + 1];
    }
}