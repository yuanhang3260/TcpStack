## TcpStack

This is just a toy :)

The main target of this project is to learn TCP protocol and implement its main
features:

- Three-way handshake and four-way disconnection.
- Reliable TCP communication based on sliding window, re-transmission timer
  and flow control.
- Congestion control.

Everything is simulated (host, TCP endpoint, layer 1 & 2 network, etc). I
implemented a very simple fake OS which only cotains network stack, and exposes
Linux-like socket APIs to user, which is able to simualte network communication
transparently.

A good example of playing with it is in main.cc which runs a very claasical
server-client model: a simple server echoing back bytes sent from a client.

<pre>
Host                 Alice                     Bob
                       |                        |
Prceoss              Client                   Server

Workflow            Socket()                  Socket()
                       |                         |
                     Bind(), optional          Bind()
                       |                         |
                    Connect()                 Listen()
                       |                         |
                       |                      Accept()
                       |                         |
                     Write()                   Read()
                       |                         |
                     Read()                    Write()
                       |                         |
                     Close()                   Close()
</pre>
  
  
#### Code
[src/Public](https://github.com/yuanhang3260/HyLib) directory points to my own C++ libraries.
