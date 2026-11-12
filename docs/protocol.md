# Miner Game Protocol (MGP) v1

自訂的二進位應用層協定，跑在 TCP 長連線上（預設 port 5555）。所有整數皆為
**big-endian（網路位元組序）**。原始碼定義於 [include/protocol.h](../include/protocol.h)。

## 1. 封包格式（Frame）

每個訊息 = 固定 12 bytes 標頭 + 變長 payload。

```
 0       1       2         3       4               8               12
+-------+-------+---------+-------+---------------+---------------+-----------
| 'M'   | 'G'   | version | type  |   seq (u32)   | length (u32)  | payload...
| 0x4D  | 0x47  |  0x01   |       |               |               | (length B)
+-------+-------+---------+-------+---------------+---------------+-----------
```

| 欄位    | 大小 | 說明 |
|---------|------|------|
| magic   | 2 B  | 固定 `4D 47`（ASCII "MG"），在 tcpdump `-X` 的 hex dump 中可一眼辨識封包邊界 |
| version | 1 B  | 協定版本，目前為 `01`；不符即視為協定錯誤 |
| type    | 1 B  | 訊息種類，見第 2 節 |
| seq     | 4 B  | 請求序號，由 client 遞增；server 的回應帶回相同 seq，server 主動推播（SNAPSHOT）為 0 |
| length  | 4 B  | payload 長度，上限 16384（`MG_MAX_PAYLOAD`）；超過即視為協定錯誤 |

### 驗證規則（強健性）
接收端收到標頭後檢查 magic、version、length 上限，任一不符：server 回傳
`ERROR(code=1 protocol)` 並關閉連線，**不會**依惡意的 length 配置記憶體或越界讀取。
例如瀏覽器誤連（`GET / HTTP/1.0`）會因 magic=`47 45` 被立即拒絕。

### TCP 串流重組（partial send / recv）
TCP 是位元組串流，不保證一次 `recv()` 剛好拿到一個 frame：
- **送出端** `mg_send_all()`（[lib/net.c](../lib/net.c)）迴圈呼叫 `send()` 直到全部送出，處理
  partial send、`EINTR`、`EAGAIN`（以 `poll(POLLOUT)` 等待），並以 `MSG_NOSIGNAL` 避免 SIGPIPE。
- **接收端** `mg_reader`（[lib/protocol.c](../lib/protocol.c)）把收到的位元組累積在緩衝區，
  只有當「12 bytes 標頭 + length bytes」到齊才取出一個完整訊息；一次收到多個 frame 也會逐一取出。

## 2. 訊息種類

| type | 名稱 | 方向 | payload |
|------|------|------|---------|
| 0x01 | HELLO        | C→S | `flags u8`（bit0 = 需要 SNAPSHOT 推播）, `name[16]`（NUL 補齊） |
| 0x02 | WELCOME      | S→C | `id u16, map_w u16, map_h u16, x u16, y u16` |
| 0x10 | MOVE         | C→S | `dir u8`（0 上, 1 下, 2 左, 3 右） |
| 0x11 | POS          | S→C | `x u16, y u16`（移動後的權威位置） |
| 0x12 | DIG          | C→S | （空）挖掘自己所在的格子 |
| 0x13 | DIG_RESULT   | S→C | `x u16, y u16, status u8, ore u8, value u32, total u32` |
| 0x20 | INV_REQ      | C→S | （空） |
| 0x21 | INV_RESP     | S→C | `count u32 × 5`（索引 0..4 = nothing/copper/silver/gold/diamond）, `total u32, digs u32` |
| 0x30 | SYSINFO_REQ  | C→S | （空） |
| 0x31 | SYSINFO_RESP | S→C | UTF-8 文字，每行 `key=value\n`（可直接在 tcpdump `-A` 中閱讀） |
| 0x40 | SNAPSHOT     | S→C | 世界快照，見第 3 節 |
| 0x50 | PING         | C→S | （空）互動 client 每 5 秒送一次，維持 idle timeout |
| 0x51 | PONG         | S→C | （空） |
| 0x60 | BYE          | C→S | （空）正常離線 |
| 0x7F | ERROR        | S→C | `code u8, text`（code: 1 protocol, 2 server full, 3 bad request, 4 timeout） |

`DIG_RESULT.status`：0 = 什麼都沒有、1 = 挖到礦（`ore`、`value` 有效）、2 = 已被挖過。
`total` 為這條連線目前累積的總價值。

## 3. SNAPSHOT 格式

```
version u32 | map_w u16 | map_h u16 | n u16 | n × (id u16, x u16, y u16) | dug bitmap
```
- `version`：世界版本號，任何可見變化（移動、挖掘、進出）都會遞增。
- dug bitmap：`ceil(map_w × map_h / 8)` bytes，格子 `i = y × map_w + x` 對應
  `byte[i / 8]` 的第 `i % 8` 位元（LSB first），1 = 已挖。
- 只對 HELLO 時設定 `flags bit0` 的 client 推送（互動 client 會設；bot 不設以節省頻寬）。
- 只在 version 改變時推送，且每條連線最多每 100 ms 一次。120×40 地圖 + 200 名玩家約 1.8 KB。

## 4. 互動流程

```
client                                server (child)
  |---- TCP 3-way handshake ------------->|  parent accept() 後 fork()
  |---- HELLO(flags,name) seq=1 --------->|  5 秒內沒收到 → 關閉
  |<--- WELCOME(id,map,pos)  seq=1 -------|  (滿員 → ERROR code=2)
  |<--- SNAPSHOT seq=0 -------------------|  (若有訂閱，之後有變化就推)
  |---- MOVE(dir) seq=2 ----------------->|
  |<--- POS(x,y)  seq=2 ------------------|
  |---- DIG seq=3 ----------------------->|  共享記憶體中加鎖 check-then-claim
  |<--- DIG_RESULT seq=3 -----------------|
  |---- SYSINFO_REQ seq=4 --------------->|
  |<--- SYSINFO_RESP seq=4 ---------------|
  |---- PING / PONG (每 5 秒) ------------|  閒置超過 15 秒 → ERROR code=4 並關閉
  |---- BYE ----------------------------->|  child 釋放玩家槽位後 _exit()
  |---- FIN / ACK ----------------------->|  parent 收到 SIGCHLD → waitpid()
```

## 5. 在封包中辨識

`tcpdump -nn -X -r logs/miner_normal.pcap` 中，TCP payload 開頭即為 `4d47 01..`：

```
0x0030:  ...  4d47 0101 0000 0001 0000 0011  ..MG............
              ^^^^ magic "MG"
                   ^^ version 1
                     ^^ type 0x01 HELLO
                        ^^^^^^^^^ seq = 1
                                  ^^^^^^^^^ length = 0x11 = 17
```

自動解碼：`python3 tools/mgp_stats.py <pcap> <port>`。
