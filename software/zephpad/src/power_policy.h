#pragma once
#include <stdbool.h>
#include <stdint.h>

/* mainだけが保持・更新する無操作タイマーの状態です。
 * generationは操作などが発生するたびに増える番号で、時刻ではありません。 */
struct zp_power_policy {
    int64_t last_activity_ms;
    uint32_t generation;
    bool initialized;
    bool linked;
    bool ready;
};

struct zp_power_sample {
    int64_t now_ms;
    uint32_t generation;
    bool linked;
    bool ready;
    bool busy; /* キー保持中、未処理の操作あり、または入力再開待ち */
};

bool zp_power_due(struct zp_power_policy *state,
                  const struct zp_power_sample *sample,
                  bool enabled, int64_t idle_ms, int64_t offline_ms);
void zp_power_postpone(struct zp_power_policy *state, int64_t now_ms);
