/**
 * @file chattyutil.c
 * @brief Implementazione delle funzioni di utilità per il server chatty
 * Si dichiara che il contenuto di questo file e' in ogni sua parte(1) opera  
 * originale dell'autore.
 * 
 * (1): le funzioni: fnv_hash_function e ulong_hash_function sono state prese
 * dal file test_hash.c presente insieme all'implementazione della tabella hash
 * fornita nell'esercizio 2 della esercitazione 12.
 * http://didawiki.di.unipi.it/doku.php/informatica/sol/laboratorio17/esercitazionib/esercitazione12
 */


#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <connections.h>
#include <chattyutil.h>


/* ---------------------------------------------------------------------- 
 * Hashing funtions
 * Well known hash function: Fowler/Noll/Vo - 32 bit version
 */
unsigned int fnv_hash_function( void *key, int len ) {
    unsigned char *p = (unsigned char*)key;
    unsigned int h = 2166136261u;
    int i;
    for ( i = 0; i < len; i++ )
        h = ( h * 16777619 ) ^ p[i];
    return h;
}

/* funzione hash per l'hashing di interi */
unsigned int ulong_hash_function( void *key ) {
	int len = sizeof(unsigned long);
	unsigned int hashval = fnv_hash_function(key,len);
    return hashval;
}

int ulong_key_compare( void *key1, void *key2  ) {
	/* Funzione di compare per le stringhe (chiavi della tabella hash). */
    return ((strncmp((char*)key1,(char*)key2,MAX_NAME_LENGTH)) == 0);
}



void freeHashData(void *data){
	/* Elimina l'elemento data dalla tabella hash e la sua history. */
	clientReg *tmp = (clientReg*) data;
	clientHist *curr;
	while((curr = popHist(tmp)) != NULL){
		free(curr->msg.data.buf);
		free(curr);
	}
	free(tmp);
}




void modHashElement(clientReg *arg_client,int fd_c, int online, char *nickname){
	/* Inizializza l'elemento arg_client nella tabella hash. */
	arg_client->fd_c = fd_c;
	arg_client->online = online;
	strncpy(arg_client->nickname,nickname,MAX_NAME_LENGTH+1);
	arg_client->head = NULL;
	arg_client->tail = NULL;
	arg_client->num_hist_msg = 0;
}


clientHist *popHist(clientReg* arg_client){
	/* Preleva la testa dalla history. */
	clientHist *tmp = arg_client->head;
	if(tmp != NULL){
		arg_client->head = tmp->next;
		/* Decrementa il numero di messaggi nella history. */
		arg_client->num_hist_msg--;
	}
	/* La free viene fatta nel worker. */
	return tmp;
}

int pushHist(clientReg* arg_client, message_t *arg_msg){
	/* Crea un nuovo nodo della history. */
	clientHist* newHist;
	CALLOC(newHist,1,clientHist,"Impossibile allocare un nuovo hist nodo\n")
	newHist->next = NULL;
	/* Copia il messaggio da salvare. */
	message_t *new = &(newHist->msg);
	CALLOC(new->data.buf,arg_msg->data.hdr.len,char,"Impossibile allocare il buffer nell'hist nodo\n")
	/* Copia l'operazione. */
	new->hdr.op = arg_msg->hdr.op;
	/* Copia il mittente. */
	strncpy(new->hdr.sender,arg_msg->hdr.sender,MAX_NAME_LENGTH+1);
	/* Copia la lunghezza del messaggio. */
	new->data.hdr.len = arg_msg->data.hdr.len;
	/* Copia il destinatario. */
	strncpy(new->data.hdr.receiver,arg_msg->data.hdr.receiver,MAX_NAME_LENGTH+1);
	/* Copia il messaggio. */
	strncpy(new->data.buf,arg_msg->data.buf,arg_msg->data.hdr.len);
	/* Incrementa il numero di messaggi nella history. */
	arg_client->num_hist_msg++;
	/* Lo aggiunge in coda alla lista. */
	if(arg_client->head == NULL){
		arg_client->head = newHist;
		arg_client->tail = arg_client->head;
	}else{
		arg_client->tail->next = newHist;
		arg_client->tail = arg_client->tail->next;
	}
	return 1;
}




/******FUNZIONI PER LA CODA CONDIVISA DELLE RICHIESTE DEI CLIENTI****************/


int push(int arg_fdc,queue *arg_queue) {
	/* Crea un nuovo nodo e lo inserisce in testa alla lista. */
	queueNode *new;
	CALLOC(new,1,queueNode,"Unable to allocate new node\n")
	new->fd_c = arg_fdc;
	new->next = NULL;
	if(arg_queue->head == NULL){
		arg_queue->head = new;
		arg_queue->tail = arg_queue->head;
	}else{
		arg_queue->tail->next = new;
		arg_queue->tail = arg_queue->tail->next;
	}
	return 1;
}


int pop(queue *arg_queue){
	/** 
	 * C'è sempre un elemento in lista quando si esegue la pop
	 * altrimenti il worker NON si sbloccherebbe dalla wait.
	 */
	int fd_c;
	queueNode *tmp = arg_queue->head;
	fd_c = arg_queue->head->fd_c;
	arg_queue->head = tmp->next;
	free(tmp);
	return fd_c;
}

void deleteQueue(queue *arg_queue){
	/* Elimina i nodi della coda delle richieste. */
	queueNode *curr = arg_queue->head;
	queueNode *tmp = NULL;
	while(curr != NULL){
		tmp = curr;
		curr = curr->next;
		free(tmp);
	}
}

/*******FUNZIONI PER LA CODA DEI FILE DESCRIPTOR ATTIVI CON NICKNAME********/

void findMax(int arg_fdc, queue *arg_queue){
	/**
	 * Scorre il set degli utenti attivi dal massimo corrente
	 * fino alla fine.
	 * NB: Si fermerà sempre al file descriptor del listener socket.
	 */
	FD_CLR(arg_fdc,&(arg_queue->set_max));
	if(arg_fdc == arg_queue->fd_num_max){
		for(int fd = arg_fdc;fd>=0;fd--){
			if(FD_ISSET(fd,&(arg_queue->set_max))){
				arg_queue->fd_num_max = fd;
				break;
			}
		}
	}
}


queueActive *searchActive(int arg_fdc,queue *arg_queue){
	/* Ricerca nella lista attivi il file descriptor arg_fdc. */
	queueActive *tmp = arg_queue->headActive;
	while(tmp != NULL){
		if(tmp->fd_c == arg_fdc) return tmp;
		else tmp = tmp->next;
	}
	return NULL;
}

queueActive *popActive(int arg_fdc,queue *arg_queue){
	queueActive *tmp = arg_queue->headActive;
	/* Sono in testa */
	if(tmp->fd_c == arg_fdc){
		/* Ritorna l'elemento in testa alla lista. */
		fprintf(stderr,"POP ACTIVE : IN TESTA. FILE DESCRIPTOR : %d\n",arg_fdc);
		arg_queue->headActive = arg_queue->headActive->next;
		return tmp;
	}else{
		/* Scorre fin quando non trova l'elemento. */
		queueActive *prec = NULL;
		while(tmp != NULL){
			if(tmp->fd_c == arg_fdc){
				fprintf(stderr,"POP ACTIVE : NEL MEZZO. FILE DESCRIPTOR : %d\n",arg_fdc);
				prec->next = tmp->next;
				return tmp;
			}else{
				prec = tmp;
				tmp = tmp->next;
			}
		}
	}
	/**
	 * Non ha trovato l'elemento oppure nella coda
	 * c'è solo il LISTENER che non viene mai eliminato.
	 */
	fprintf(stderr,"POP ACTIVE : NON TROVATO. FILE DESCRIPTOR : %d\n",arg_fdc);
	return NULL;
}



int insertActive(int arg_fdc, char* arg_nickname,queue *arg_queue){
	/* Inizializza un nuovo elemento. */
	queueActive *new;
	CALLOC(new,1,queueActive,"Impossibile allocare nuovo nodo")
	//memset(new->nickname,0,MAX_NAME_LENGTH+1);
	new->fd_c = arg_fdc;
	strncpy(new->nickname,arg_nickname,strlen(arg_nickname)+1);
	new->next = NULL;
	if(arg_queue->headActive == NULL){
		/* Viene inserito il primo elemento nella lista cioè il LISTENER. */
		fprintf(stderr,"INSERT ACTIVE : INSERITO IN TESTA. UTENTE : %s FILE DESCRIPTOR : %d\n",arg_nickname,arg_fdc);
		arg_queue->headActive = new;
	}else{
		queueActive *prec = NULL;
		queueActive *curr = arg_queue->headActive;
		/* Scorre la lista per inserirlo in ordine descrescente di fd_c*/
		while(curr != NULL && curr->fd_c > arg_fdc){
			prec = curr;
			curr = curr->next;
		}
		if(prec != NULL){
			/* Viene inserito nel mezzo della lista. */
			fprintf(stderr,"INSERT ACTIVE : INSERITO NEL MEZZO. UTENTE : %s FILE DESCRIPTOR : %d\n",arg_nickname,arg_fdc);
			prec->next = new;
			new->next = curr;
		}else{ 
			/* Viene inserito in testa alla lista. */
			fprintf(stderr,"INSERT ACTIVE : INSERITO IN TESTA. UTENTE : %s FILE DESCRIPTOR : %d\n",arg_nickname,arg_fdc);
			new->next = curr;
			arg_queue->headActive = new;
		}
	}
	return 1;
}



void deleteActiveQueue(queue *arg_queue){
	queueActive *curr = arg_queue->headActive;
	queueActive *tmp = NULL;
	while(curr != NULL){
		tmp = curr;
		curr = curr->next;
		free(tmp);
	}
}


/*********FUNZIONI DI UTILITÀ*********************/

int initThreadParams(config *arg_params, queue *arg_queue){
	/* Inizializza testa e coda della lista richieste. */
	arg_queue->head = NULL;
	arg_queue->tail = NULL;
	arg_queue->fd_num_max = -1;
	/* Resetta la maschere per i file descriptor delle richieste/attivi. */
	FD_ZERO(&(arg_queue->set));
	FD_ZERO(&(arg_queue->set_max));
	/* Inizializza il puntatore alla lista utenti attivi. */
	arg_queue->headActive = NULL;
	arg_queue->params = arg_params;
	/* Crea la cartella per i file se questa non esiste. */
	struct stat st;
	if(stat(arg_params->DirName,&st) == -1){
		if(mkdir(arg_params->DirName,0700) == -1){
			perror("Errore nel creare la directory");
			return -1;
		}
	}
	/* Crea la tabella hash. */
	if((arg_queue->hash = icl_hash_create(N_BUCKETS, ulong_hash_function, ulong_key_compare)) == NULL){
		fprintf(stderr,"Errore nel creare la tabella hash\n");
		return -1;
	}
	return 1;
}


int checkParams(config *params){
	if((params->ThreadsInPool) > MAX_THREAD) params->ThreadsInPool = MAX_THREAD;
	if(strlen(params->UnixPath) > UNIX_PATH_MAX){
		fprintf(stderr,"CHECK PARAMS : SOCKET PATH SUPERA LIMITE MASSIMO. CURR PATH : %zu MAX PATH : %d\n",strlen(params->UnixPath),UNIX_PATH_MAX);
		deleteConfig(params);
		return -1;
	}
	return 1;
}



int maskFunction(sigset_t *set){
	/* Azzera tutta la maschera. */
	MINUS(sigemptyset(set),"Impossibile azzerare il set.")
	/* Maschera SIGINT. */
	MINUS(sigaddset(set,SIGINT),"Impossibile mascherare SIGINT.")
	/* Maschera SIGQUIT. */
	MINUS(sigaddset(set,SIGQUIT),"Impossibile mascherare SIGQUIT.")
	/* Maschera SIGTERM. */
	MINUS(sigaddset(set,SIGTERM),"Impossibile mascherare SIGTERM.")
	/* Maschera SIGUSR1. */
	MINUS(sigaddset(set,SIGUSR1),"Impossibile mascherare SIGUSR1.")
	/* Maschera SIGPIPE. */
	MINUS(sigaddset(set,SIGPIPE),"Impossibile mascherare SIPIPE.")
	/* Installa la maschera. */
	NOTZERO(pthread_sigmask(SIG_SETMASK,set,NULL),"Impossibile installare la maschera.")
	return 1;
}



int socketInit(struct sockaddr_un *arg_sock ,config *params){
	/* File descriptor listen. */
	int fd_skt;
	/* Address della bind. */
	struct sockaddr_un sa = *arg_sock;
	/* Inizializza il path della socket. */
	strncpy(sa.sun_path,params->UnixPath,UNIX_PATH_MAX+1);
	/* Socket di tipo AF_UNIX. */
	sa.sun_family = AF_UNIX;
	/* Crea la socket. */
	if((fd_skt = socket(AF_UNIX,SOCK_STREAM,0)) == -1){
		perror("Impossibile creare la socket.");
		return -1;
	}
	/* Assegna l'address alla socket precedentemente creata. */
	MINUS(bind(fd_skt,(struct sockaddr *) &sa,sizeof(sa)),"Unable to bind.")
	/* Marco la socket come "passiva" cioè in attesa di connessioni in entrata. */
	MINUS(listen(fd_skt,params->MaxConnections),"Something goes wrong while listening.")
	return fd_skt;
}

	
void initOnlineList(queueActive *arg_queue_active, char* arg_online_list){
	int i =0;
	int j = 0;
	/* Indice per i nickname nella coda attivi. */
	int k = 0;
	/* Lunghezza del nickname in arg_queue_active. */
	int len ;
	/* Lista degli utenti attualmente attivi. */
	queueActive * curr = arg_queue_active;
	while(curr != NULL){
		if(strncmp(curr->nickname,"",MAX_NAME_LENGTH+1) != 0){
			/* Se è diverso dal nickname speciale. */
			len = (int) strlen(curr->nickname);
			k=0;
			/* Ricopia tutto il nickname. */
			for(i = j;i< j + len;i++){
				arg_online_list[i] = curr->nickname[k];
				k++;
			}
			/* Aggiunge degli spazi per arrivare a MAX_NAME_LENGTH. */
			for(i = j + len;i< j + (MAX_NAME_LENGTH);i++){
				arg_online_list[i] = ' ';
			}
			/* Aggiunge il carattere terminatore. */
			arg_online_list[i] = '\0';
			/* Incrementa per posizionarsi alla fine della stringa. */
			j += MAX_NAME_LENGTH +1;
			
		}
		curr = curr->next;
	}
}

int createSubDir(int fd_dir,char *arg_path){
	/* Duplica la stringa in modo da evitare modifiche. */
	char *path = strdup(arg_path);
	/* Lunghezza della stringa senza carattere terminatore. */
	int len = strlen(path);
	int i = len;
	int j = 0;
	/* Se comincia con ./ evita di leggere il punto */
	if(path[0] == '.') {j = 1; }
	/** 
	 * Calcola la nuova lunghezza senza contare il nome del file.
	 * partendo dalla fine della stringa fino ad arrivare al primo '/'.
	 * Se non vi è alcun '/' non fa nulla.
	 */
	while(len >= 0 && path[i] != '/'){
		i--;
		len--;
	}
	/* Inserisce il terminatore nel punto dove inizia il nome del file. */
	path[i+1] = '\0';
	/**
	 * Partendo dall'inizio se incontra '/' cioè la fine di un nome di una directory
	 * lo sostituisce con il terminatore '\0' e crea la directory fin a quel momento
	 * se non esiste.
	 */ 
	for(i =j;i<=len;i++){
		if(path[i] == '/'){
			/* Inserisco il terminatore per poter fare mkdir */
			path[i] = '\0';
			if(mkdirat(fd_dir,path,0700) == -1){
				if(errno != EEXIST){
					perror("Impossibile creare le directory");
					free(path);
					return -1;
				}
			}
			/* Inserisce il carattere '/' per poter continuare il path */
			path[i] = '/';
		}
	}
	free(path);
	return 1;
}
