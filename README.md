# assignment


first use scipt to check whats in dbn file
then use more detailed script to check the content orgnisation

insatll data bento lib
use scitp to prperly read data (linking databento lib)

bulid a minimal pipile from read data - strame - order book



| Field          | Description                                                                                                  |
|----------------|--------------------------------------------------------------------------------------------------------------|
| ts_recv        | Time when the message was received by DataBento (UTC, nanoseconds).                                          |
| ts_event       | Actual event time from the exchange (e.g., order added, changed, or deleted).                                |
| rtype          | Record type — tells what kind of message it is (add, modify, delete, trade, etc.).                           |
| publisher_id   | ID of the data source or exchange feed.                                                                      |
| instrument_id  | Internal ID for the contract (e.g., CLX5 → 432669).                                                          |
| action         | Action performed — usually “add”, “modify”, or “delete”.                                                     |
| side           | Order side — “B” for buy, “S” for sell.                                                                      |
| price          | Order price.                                                                                                 |
| size           | Order size (quantity).                                                                                       |
| channel_id     | Feed channel ID from the exchange.                                                                           |
| order_id       | Unique ID of the order in the order book.                                                                    |
| flags          | Extra system flags or status indicators.                                                                     |
| ts_in_delta    | Time difference (in nanoseconds) between this and the previous event.                                        |
| sequence       | Sequential message number, used to keep correct event order.                                                 |
| symbol         | The contract symbol (e.g., “CLX5”).                                                                          |


Columns: ['ts_event', 'rtype', 'publisher_id', 'instrument_id', 'action', 'side', 'price', 'size', 'channel_id', 'order_id', 'flags', 'ts_in_delta', 'sequence', 'symbol']

Unique symbols: ['CLX5']
Time range: 2025-09-24 19:30:00.001385399+00:00 → 2025-09-24 21:59:59.999903747+00:00

action
C    16252 cancel order
A    14959 add order 
F     2701 fill order
M     2701 modify order
T     1599 trade summery 

--- Side (Buy/Sell) ---
side
A    18967 
B    18949
N      296
Name: count, dtype: int64

Average inter-message gap (ms): 235.53423146078356
Max gap (ms): 2700049.415731

