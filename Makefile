server: main.cpp ./skiplist/node.h ./skiplist/skiplist.h ./threadpool/threadpool.h ./http/http_conn.h ./http/http_conn.cpp ./lock/locker.h ./log/block_queue.h ./log/log.h ./log/log.cpp ./CGI_MySQL/sql_connection_pool.h ./CGI_MySQL/sql_connection_pool.cpp
	g++ -o server main.cpp ./skiplist/node.h ./skiplist/skiplist.h ./threadpool/threadpool.h ./http/http_conn.h ./http/http_conn.cpp ./lock/locker.h ./log/block_queue.h ./log/log.h ./log/log.cpp ./CGI_MySQL/sql_connection_pool.h ./CGI_MySQL/sql_connection_pool.cpp -lpthread -lmysqlclient
clean:
	rm -r server