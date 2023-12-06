all:
	cc new_alarm_cond.c -D_POSIX_PTHREAD_SEMANTICS -lpthread -lm
	./a.out