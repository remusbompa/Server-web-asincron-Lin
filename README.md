# Asynchronous web server for passing files to clients

## Puropose

Implemented a web server that uses advanced I/O operations:
* asynchronous operations on files
* non-blocking operations on sockets
* zero-copying
* multiplexing I/O operations

The implementation uses the API of advanced I/O operations specific to the Linux operating:
* sendfile
* io_setup & friends
* epoll

The web server uses the modern multiplexing API to wait for connections from clients: epoll (Linux). On the established connections, requests from clients are received and then responses are distributed to them.

Clients and server communicate using the HTTP protocol. For parsing HTTP requests from clients I am using an HTTP parser. I have used a callback to get the path to the local resource requested by the client.

The server implements a limited functionality of the HTTP protocol, that of passing files to clients. The server serves files from the AWS_DOCUMENT_ROOT directory, defined within the theme header. Files are only found in subdirectories AWS_DOCUMENT_ROOT/static/, respectively AWS_DOCUMENT_ROOT/dynamic/, and corresponding request paths are, for example, AWS_DOCUMENT_ROOT/static/test.dat, respectively AWS_DOCUMENT_ROOT/dynamic/test.dat. File processing is as follows:
* The files in the AWS_DOCUMENT_ROOT/static/ directory are static files that are transmitted to clients using the zero-copying API ( sendfile).
* Files in the AWS_DOCUMENT_ROOT/dynamic/ directory are files that are supposed to require a server-side post-processing phase. These files are read from disk using the asynchronous API and then pushed to the clients. Streaming uses non-blocking sockets.
* An HTTP 404 message is sent for invalid request paths.

After transmitting a file, according to the HTTP protocol, the connection is closed.


## Explanations

Communication in the network, from client to server, is done in a non-blocking way using non-blocking sockets: when creating a new connection, the call is used <em>fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK)</em>
* in the case of static files, non-blocking is sent directly from the kernel using zero API copying <em>sendfile</em>
* in the case of dynamic files, the data is sent asynchronously using functions from the <em>io_setup</em> family
* receiving notifications about the possibility of sending/receiving on non-blocking sockets and of the completion of asynchronous operations is done through <em>epoll</em>

There are 3 cases in which the program can be, each with certain states:
* initially the program is in case 0, and upon receiving a request from a client it is called function <em>handle_client_request</em>
* the function analyzes requests by calling <em>receive_request</em> and if it returns STATE_DATA_RECEIVED, check the path saved in <em>request_path</em>:
1. is not valid => case 0
2. prefixed with the static directory => case 1
3. prefixed with the dynamic directory => case 2
4. does not have a valid prefix => case 0
5. the function returns the socket to epoll with the POLLOUT flag
6. a POLLOUT notification is received and it is checked which case they are:
 case 0 => I send an error message and close the connection
* case 1 => I send the message header and go to state 1. In state 1 I send with sendfile
how many BUFSIZ bytes until it ends and then close the connection
* case 1 => I send the message header and go to state 1. In state 1 the function is called <em>send_file_aio</em> which allocates a sufficient number of buffers to cover the size file together, each buffer will read asynchronously through the call <em>io_prep_pread</em> and then <em>io_submit</em> a certain segment from the file, state 2 is entered. Because with an io_submit call not necessarily all read operations in buffers are submitted, this state can be repeat. In state 2, the next buffer, in which the reading was completed, is sent to the socket. When as many buffers have been received as have been sent, it means that all segments have been sent submitted, and another submission is made. When the total number of buffers has been sent, it closes connection

## How to compile and run the application ?

I have used the following files:

* http_parser.c, http_parser.h
* sock_util.c, sock_util.h
* debug.h, util.h, w_epoll.h, aws.h

Makefile generates first the object files for http_parser.c, sock_util.c si aws.c

The generated 3 files are then linked with libaio to use its asynchronous functions

The executable is run from terminal <em>./aws</em> and open another terminal to send HTTP request for files to the server.
