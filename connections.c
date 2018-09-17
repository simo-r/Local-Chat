/**
 * @file connections.c
 * @brief funzioni di comunicazione client/server.
 * Si dichiara che il contenuto di questo file e' in ogni sua parte opera  
 * originale dell'autore.
 */


#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <connections.h>



int openConnection(char* path, unsigned int ntimes, unsigned int secs){
	/* La gestione dell'errore avviene nel client. */
	if(strlen(path) > UNIX_PATH_MAX) return -1;
	/* Non fallisce ma setta il numero di tentativo al massimo. */
	if(ntimes > MAX_RETRIES) ntimes = MAX_RETRIES;
	/* Non fallisce ma viene settato al massimo tempo di sleep. */
	if(secs > MAX_SLEEPING) secs = MAX_SLEEPING;
	/* File descriptor della connessione. */
	int fd_skt;
	struct sockaddr_un sa;
	strncpy(sa.sun_path,path,UNIX_PATH_MAX);
	sa.sun_family = AF_UNIX;
	if((fd_skt = socket(AF_UNIX,SOCK_STREAM,0)) == -1){
		perror("Errore nel creare il file descriptor per il nuovo socket");
		return -1;
	}
	/* Riprovo a connettermi fin quando non ricevo risposta/esaurisco i tentativi. */
	while((ntimes != 0) && (connect(fd_skt,(struct sockaddr*) &sa,sizeof(sa)) == -1)){
		/* Il server non ha ancora accettato la richiesta. */
		if(errno == ENOENT){
			sleep(secs); 
		}else{
			perror("Impossibile connettersi");
			return -1;
		}
		ntimes--;
	}
	if(ntimes == 0) {
		fprintf(stderr,"Numero massimo di tentativi superato\n");
		return -1;
	}
	return fd_skt;
}



int readHeader(long connfd, message_hdr_t *hdr){
	/* Valore di ritorno della read. */
	int r;
	/* Leggo l'intero contenuto della struttura in una sola volta. */
	r = read(connfd,hdr,sizeof(message_hdr_t));
	if(r == 0){
		fprintf(stderr,"La connessione è stata chiusa\n");
		return 0;
	}else if( r == -1){
		/* Evito di essere terminato da un SIGPIPE. */
		if(errno != ECONNRESET && errno!= EBADF){
		/* errno è settato automaticamente dal fallimento della read. */
			perror("Lettura dell'header fallita");
			return -1;
		}
	}
	return 1;
}



int readData(long fd, message_data_t *data){
	/* Valore di ritorno della read. */
	int r;
	/* Leggo l'header della parte dati per conoscere anche la dimensione. */
	if((r = read(fd,&(data->hdr),sizeof(message_data_hdr_t))) == -1){
		/* Evito di essere terminato da un SIGPIPE. */
		if(errno != ECONNRESET && errno != EBADF){
			perror("Errore nella lettura");
			return -1;
		}
	}else if(r == 0){
		fprintf(stderr,"La connessione è stata chiusa\n");
		return 0;
	}
	/* Alloco il buffer della giusta dimensione. */
	/* Nei casi REG,CONN,USR e altri la size è 0 ritornando NULL anche se non è un errore */
	if(data->hdr.len > 0){
	if((data->buf = calloc((data->hdr.len),sizeof(char))) == NULL){
		fprintf(stderr,"Calloc fallita\n");
		return 0;
	}
	/* Leggo il messaggio che mi è stato inviato. */
	if((r = read(fd,data->buf,data->hdr.len)) == -1){
		/* Evito di essere terminato da un SIGPIPE. */
		if(errno != ECONNRESET && errno != EBADF){
			perror("Errore nella lettura");
			return -1;
		}
	}else if(r == 0){
		fprintf(stderr,"La connessione è stata chiusa\n");
		return 0;
	}
	//fprintf(stderr," IL MESSAGGIO DENTRO LA READDATA È : %s\n",data->buf);
	}else data->buf = NULL;
	return 1;
}



int readMsg(long fd, message_t *msg){
	/* Valore di ritorno della read. */
	int r;
	/* Leggo l'header del messaggio. */
	if((r=readHeader(fd,&(msg->hdr))) == 0) return 0;
	else if( r == -1) return -1;
	/* Leggo il body del messaggio. */
	if((r = readData(fd,&(msg->data))) == 0) return 0;
	else if(r == -1) return -1;
	return 1;
}



int sendRequest(long fd, message_t *msg){
	/* Mando l'header del messaggio. */
	if(write(fd,&msg->hdr,sizeof(message_hdr_t)) == -1){
		/* Evito di essere terminato da un SIGPIPE. */
		if(errno != EBADF && errno != EPIPE){ 
			perror("Errore nella scrittura");
			return -1;
		}
	}
	/* Mando la parte dei dati. */
	if(sendData(fd,&(msg->data)) == -1){
		return -1;
	}
	return 1;
}



int sendData(long fd, message_data_t *msg){
	/* Scrive prima l'header e poi il messaggio. */
	struct iovec iov[2];
	/* Inizializzo la base al puntatore all'inizio della struttura. */
	iov[0].iov_base = &(msg->hdr);
	/* Inizializzo la lunghezza (da leggere) all'intera struttura. */
	iov[0].iov_len = sizeof(message_data_hdr_t);
	/* Inizializzo la base al puntatore all'inizio della stringa. */
	iov[1].iov_base = msg->buf;
	/* Inizializzo la lunghezza (da leggere) all'intera stringa. */
	iov[1].iov_len = msg->hdr.len;
	/* Faccio le due write. */
	if(writev(fd,iov,2) == -1){
		/* Evito di essere terminato da un SIGPIPE. */
		if(errno != EPIPE && errno != EBADF){
			perror("Errore nella scrittura2");
			return -1;
		}
	}
	return 1;
}
