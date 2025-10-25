# assignment

Ai tools used
    ChatGtp and cursor

    gpt has more contextaual knowledge so better to use when have athceture deisgn 
    curosr is beeter for pure coding logic/systax question

1.5 hr ~ 2h

1. use script to check whats in dbn file, start with python for convenience. 

2. then use more detailed script to check the content organisation

3. after knowing what in it, change to use c++, use a short script to test reading the file 

4. after a few attempts it keeping reading random characters. consulted with chatgpt, databento has its own lib

5. download and compile databento lib, manage to link to the script and read file correctly

6. after the script can read file properly, let cursor generate a minimal pipeline, follow the same file reading approach

7. use databento official order book sample, streamer to read file then pass into order book. single thread, no tcp

problem : many order id are not found or price level event not found, since the data start from 19:30 its possible some cancel or modify request are using order that don't exist


20 mins

quikc demo to do consuer producer to send mesge throuth tcp. initialy 40k~ messsge / sec. after apply ing some optimise tirck no improvement
suspect the bottonle neck in IO. used zroe copy and cpu chche pripritsue, still no improment

since im testing on local machine so although ringbuffer queue will help but also not implemted

zero-copy technique did not help with performce, so removed as its casuing bug while toghert with batching, csauing memery aligment issues

no preforame boost come from bathcing too

20 mins

updatet the previous mnimal approch, break it up into sneder reciver and process order book on receiver side same final order boook state as minial approch

20 min 

added json output feature into order book, standerad metics caltulation and output on sedner and receiver


=== TCP Receiver Final Statistics ===
Processing Time: 114 ms
Messages Received: 38212
Orders Processed: 36988
Message Throughput: 335193 messages/sec
Order Processing Rate: 324456 orders/sec

=== Final Statistics ===
Processing Time: 114 ms
Processed Orders: 36988
Processing Rate: 324456 orders/sec
Final Order Book:
  Total Orders: 147
  Bid Levels: 61
  Ask Levels: 52
  Best Bid: 64 @ 3 (1 orders)
  Best Ask: 65 @ 1 (1 orders)
  Spread: 620000000


=== Final Timing Statistics ===
Total Time: 1005 ms
Total Messages: 38212
Average Latency: 29.485 ms
Min Latency: 0.120 ms
Max Latency: 43.512 ms
Messages/sec: 38022




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

