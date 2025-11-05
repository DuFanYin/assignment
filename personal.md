# assignment

AI tools used
    ChatGPT and Cursor

    GPT has more contextual knowledge so better to use when have architecture design 
    Cursor is better for pure coding logic/syntax questions, auto for majority of time, use Claude 4.5 when JSON bug couldn't be fixed

1.5 hr ~ 2h

1. use script to check what's in dbn file, start with python for convenience. 

2. then use more detailed script to check the content organization

3. after knowing what in it, change to use c++, use a short script to test reading the file 

4. after a few attempts it kept reading random characters. consulted with chatgpt, databento has its own lib

5. download and compile databento lib, manage to link to the script and read file correctly

6. after the script can read file properly, let cursor generate a minimal pipeline, follow the same file reading approach

7. use databento official order book sample, streamer to read file then pass into order book. single thread, no tcp

problem : many order id are not found or price level event not found, since the data start from 19:30 it's possible some cancel or modify request are using order that don't exist


20 mins

quick demo to do consumer producer to send message through tcp. initially 40k~ message / sec. after applying some optimize trick no improvement
suspect the bottleneck in IO. used zero copy and cpu cache priority, still no improvement

since I'm testing on local machine so although ringbuffer queue will help but also not implemented

zero-copy technique did not help with performance, so removed as it's causing bug while together with batching, causing memory alignment issues

no performance boost come from batching too

20 mins

updated the previous minimal approach, break it up into sender receiver and process order book on receiver side same final order book state as minimal approach

20 min 

added json output feature into order book, standard metrics calculation and output on sender and receiver

10 min

batch write json to reduce IO overhead

30 min 

tried using ringbuffer for IO buffer, no improvement

40 mins

python server to connect c++ sender and receiver(order book)

50 mins

containerize 3 micro services, too a while to debug cos configuration don't match

30 mins

to fix metrics calculation and display bug

20 mis

fro server version, add ring buffuer to decouple json generation and order book process
huge performace boost

30 min 

fix bugs and apply ring buffer to microservice version

1hr

implement cpp 20 standard on src version, and other minor tuning, clean up




thoughts

most of the concepts in the assignment is not new for me although I haven't had chance to use some of them in real projects. In terms of design I made a lot of assumption how a real system would work. (eg loha.botonics)

basically the approach to build this is to start from simple minimal pipeline to build it up step by step, in the end when the json saving bug and display bug happened, just use older and simpler version to identify the error so it's easier to know what went wrong

microservice version the performance dropped significantly I assume coming from container overhead

both server and microservice version will send entire json from backend, using a 4th service to create database and volume, batching storing and write into db will be much faster in response time

also server implementation is extremely simple, for expansion would let python server spin up multiple order book instances and have a sender to send data depending on time frame and other parameters.

for other minor tasks such as test/ci/cd, i have used those in my project before, for the purpose of this assignment I focused on more architecture related parts, with enough time I can definitely do all of them
