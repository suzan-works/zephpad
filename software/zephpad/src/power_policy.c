#include "power_policy.h"

void zp_power_postpone(struct zp_power_policy *state, int64_t now_ms)
{
    state->last_activity_ms = now_ms;
}

bool zp_power_due(struct zp_power_policy *s, const struct zp_power_sample *p,
                  bool enabled, int64_t idle_ms, int64_t offline_ms)
{
    /* 操作や接続状態の変化があれば、無操作時間を最初から数えます。 */
    bool changed = !s->initialized || s->generation != p->generation ||
                   s->linked != p->linked || s->ready != p->ready;
    s->initialized = true;
    s->generation = p->generation;
    s->linked = p->linked;
    s->ready = p->ready;

    /* 接続済みで暗号化・HID通知の準備中はOFFしません。
     * 入力準備が完了した時点から、接続中の無操作時間を数えます。
     * 時刻の逆転や一時的に矛盾する状態も、即OFFと誤判定しません。 */
    if (!enabled || changed || p->busy || (p->linked && !p->ready) ||
        p->now_ms < s->last_activity_ms || (p->ready && !p->linked)) {
        zp_power_postpone(s, p->now_ms);
        return false;
    }
    /* 接続中と未接続時の設定は独立。判定ロジックだけ共通です。 */
    int64_t timeout = p->ready ? idle_ms : offline_ms;
    return timeout > 0 && p->now_ms - s->last_activity_ms >= timeout;
}
