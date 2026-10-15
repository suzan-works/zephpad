#pragma once

/* 時計回りでエフォートが下がる場合は -1 に変更します。 */
#define ZEPHPAD_ENCODER_SIGN (+1)

/* 単発ショートカットの押下・解放間隔。実機確認済みの値です。 */
#define ZEPHPAD_TAP_MS 20

/* 接続中と未接続時の待ち時間は、同じ5分でも独立して設定します。
 * LL は64ビット整数。Zephyrの起動後時刻（ミリ秒）と合わせます。
 * キー保持中と、BLE接続済みで暗号化/HID通知準備中はOFFしません。 */
#define ZEPHPAD_IDLE_OFF_MS    (5LL * 60 * 1000)
#define ZEPHPAD_OFFLINE_OFF_MS (5LL * 60 * 1000)

/* 通常は1。自動OFFの切り分け試験だけ0に変更できます。 */
#define ZEPHPAD_AUTO_OFF 1

/* 入力準備完了後3秒点灯。待機中は500ms点灯＋500ms消灯＝1Hz。 */
#define ZEPHPAD_CONNECTED_LED_MS 3000LL
#define ZEPHPAD_WAIT_LED_HALF_PERIOD_MS 500LL
