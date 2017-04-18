# TcpStack

This is just a toy :)

The main purpose of this project is to learn TCP protocol and implement its main functions:

- Three-way handshake and four-way disconnection.
- Reliable communication based on slide window, re-transmission and flow control.
- Congestion control.

Everything is simulated (host, TCP endpoint, layer 1 & 2 network, etc). It exposes socket-like APIs to user and simualtes network traffic transparently. A good example of playing with it is in main.cc which simulates a server echoing back bytes sent from a client.
