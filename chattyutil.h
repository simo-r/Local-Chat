/**
 * @file chattyutil.h
 * @brief Header di utilità per il server chatty
 * Si dichiara che il contenuto di questo file e' in ogni sua parte(1) opera  
 * originale dell'autore.
 * 
 * (1): le funzioni: fnv_hash_function e ulong_hash_function sono state prese
 * dal file test_hash.c presente insieme all'implementazione della tabella hash
 * fornita nell'esercizio 2 della esercitazione 12.
 * http://didawiki.di.unipi.it/doku.php/informatica/sol/laboratorio17/esercitazionib/esercitazione12
 */

#if !defined(CHATTYUTIL_H)
#define CHATTYUTIL_H

#define _POSIX_C_SOURCE 200809L

#include <parser.h>
#include <stdio.h>
#include <signal.h>
#include <sys/un.h>
#include <string.h>
#include <sys/select.h>
#include <connections.h>
#include <config.h>
#include <icl_hash.h>


#define MAX_THREAD 20
#define N_BUCKETS 1024
#define N_MUTEX_HASH 256

#define CLEAREXIT \
	free(msg_in); \
	free(hdr_out); \
	if(closedir(filedir) == -1) perror("Impossibile chiudere la file directory");

#define UPDATESTAT(statp,statm) \
	pthread_mutex_lock(&mtx_stat); \
	statp++; \
	if(statm != 0){ statm--; }\
	pthread_mutex_unlock(&mtx_stat);

#define INCRSTAT(stat) \
	pthread_mutex_lock(&mtx_stat); \
	stat++; \
	pthread_mutex_unlock(&mtx_stat);

#define LOCKSET(fd) \
	pthread_mutex_lock(&mtx_set); \
	FD_SET(fd, set); \
	pthread_mutex_unlock(&mtx_set);	


#define MINUS(c,e) \
   	if((c)==-1) { perror(e); return -1; }


   	
#define NOTZERO(c,e) \
	if((c) != 0) { perror(e); return -1; }


	
#define CALLOC(r,size,type,e) \
	if(((r) = calloc((size),sizeof(type))) == NULL){ \
		fprintf(stderr,e); \
		return -1; \
	}

		
		
#define CLOSE(fd,e) \
	if(close(fd) == -1) { perror(e); return (void*) -1; }

	
static void inline usage(const char *progname) {
    fprintf(stderr, "Il server va lanciato con il seguente comando:\n");
    fprintf(stderr, "  %s -f conffile\n", progname);
}


	
/**	
 * @struct queueNode
 * @brief Nodo della coda delle richieste
 * 
 * @var fd_c descrittore della connessione
 * @var next puntatore al prossimo elemento della coda
 */
typedef struct _queueNode{
	int fd_c;
	struct _queueNode *next;
} queueNode;

/**
 * @struct queueActive
 * @brief Associa il file descriptor all'utente
 * 
 * @var fd_c descrittore della connessione
 * @var nickname nome dell'utente
 * @var next puntatore al prossimo elemento della lista
 * 
 */
typedef struct _queueActive{
	int fd_c;
	char nickname[MAX_NAME_LENGTH +1];
	struct _queueActive *next;
}queueActive;


/**
 * @struct clientHist
 * @brief Nodo della lista dei messaggi pendenti. (history)
 * 
 * @var msg informazioni sul messaggio pendente.
 * @var next puntatore al prossimo elemento della lista
 * 
 */
typedef struct _clientHist{
	message_t msg;
	struct _clientHist *next;
}clientHist;


/**
 * @struct clientReg
 * @brief Elemento della tabella hash.
 * 
 * @var online flag se l'utente è online.
 * @var fd_c descrittore della connessione.
 * @var nickname nome dell'utente registrato.
 * @var head testa della lista dei messaggi pendenti.
 * @var tail coda della lista dei messaggi pendenti.
 * @var num_hist_msg numero di elementi nella lista dei messaggi pendenti.
 */
typedef struct _register{
	int online;
	int fd_c;
	char nickname[MAX_NAME_LENGTH+1];
	clientHist *head;
	clientHist *tail;
	size_t num_hist_msg;
}clientReg;

/**	
 *
 * @struct queue
 * @brief Struttura contentente informazioni per la gestione dei client.
 * 
 * @var head testa della lista delle richieste.
 * @var tail coda della lista delle richieste.
 * @var fd_num_max massimo file descriptor attivo.
 * @var set maschera di bit per le richieste dei client.
 * @var set_max maschera per mantenere i file descriptor attivi.
 * @var headActive testa della lista che associa file descriptor a nickname online.
 * @var params Parametri del server.
 * @var hash Tabella hash per la gestione dei clienti registrati/attivi.
 */
typedef struct _queue{
	queueNode	*head; 
	queueNode	*tail;
	int fd_num_max;
	fd_set set;
	fd_set set_max;
	queueActive *headActive;
	config *params;
	icl_hash_t *hash;
} queue;


/***************** FUNZIONI SULLA TABELLA HASH ***************************/

/* Funzione presa dal sorgente del file all'inizio citato. */
unsigned int ulong_hash_function( void *key );
/**
 * @function ulong_key_compare
 * @brief Confronta due chiavi della tabella hash.
 * 
 * @param key1 chiave della tabella. (stringhe)
 * @param key2 chiave della tabella. (stringhe)
 */
int ulong_key_compare( void *key1, void *key2  );

/**
 * @function modHashElement
 * @brief Inizializza l'elemento da inserire nella tabella hash
 * 
 * @param arg_client elemento da inizializzare
 * @param fd_c file descriptor del client
 * @param online status del client
 * @param nickname nome dell'utente associato ad fd_c
 * 
 */
void modHashElement(clientReg *arg_client,int fd_c, int online, char *nickname);

/**
 * @function freeHashData
 * @brief Elimina il contenuto della tabella hash
 * 
 * @param data elemento nella tabella hash
 * 
 * Richiama la funzione popHist eliminando il contenuto della history
 * e poi esegue la free dell'elemento.
 */
void freeHashData(void *data);

/**
 * @function popHist
 * @brief Preleva il primo elemento della lista dei messaggi pendenti.
 * 
 * @param arg_client elemento della tabella hash (utente)
 * 
 * @return elemento in testa alla lista o NULL nel caso sia vuota
 */
clientHist *popHist(clientReg* arg_client);

/**
 * @function pushHist
 * @brief Aggiunge in coda alla lista dei messaggi pendenti
 * 
 * @param arg_client elemento della tabella hash
 * @param arg_msg messaggio da aggiungere alla lista pendenti.
 * 
 * @return 1 se non ci sono errori, -1 altrimenti
 */
int pushHist(clientReg* arg_client, message_t *arg_msg);


/******FUNZIONI PER LA CODA CONDIVISA DELLE RICHIESTE DEI CLIENTI****************/


/**
 * @function push
 * @brief Aggiunge alla fine della coda richieste.
 * 
 * @param arg_fdc descrittore della connessione.
 * @param arg_queue struttura che contiene le informazioni condivise.
 * 
 * @return 1 se se non ci sono errori altrimenti -1.
 */
int push(int arg_fdc, queue *arg_queue);



/**
 * @function pop
 * @brief Elimina dalla coda il primo elemento della coda richieste.
 * 
 * @param arg_queue struttura che contiene le informazioni condivise.
 * 
 * @return fd_c .
 */
int pop(queue *arg_queue);


/**
 * @function deleteQueue
 * @brief Elimina tutti i nodi della coda richieste.
 * 
 * @param arg_queue struttura che contiene le informazioni condivise.
 */
void deleteQueue(queue *arg_queue);

/*******FUNZIONI PER LA CODA DEI FILE DESCRIPTOR ATTIVI CON NICKNAME********/


/**
 * @function findMax
 * @brief Trova il nuovo massimo file descriptor attivo.
 * 
 * @param arg_fdc file descriptor dell'utente
 * @param arg_queue struttura che contiene il set degli utenti attivi.
 *
 * Fa il clear di set_max[arg_fdc]. Se arg_fdc era il massimo trova
 * il nuovo massimo scorrendo set_max e lo assegna al valore corrente
 * del massimo arg_queue->fd_num_max
 */
void findMax(int arg_fdc, queue *arg_queue);

/**
 * @function searchActive
 * @brief Cerca il file descriptor nella lista degli utenti attivi.
 * 
 * @param arg_fdc file descriptor da ricercare
 * @param arg_queue struttura contenente la lista degli utenti attivi
 * 
 * @return elemento con file descriptor uguale. Altrimenti NULL.
 */
queueActive *searchActive(int arg_fdc,queue *arg_queue);
/**
 * @function popActive
 * @brief Elimina l'elemento della coda (se presente) con lo stesso file descriptor arg_fdc
 * 
 * @param arg_fdc descrittore della connessione da eliminare.
 * @param arg_queue struttura che contiene la lista degli utenti attivi.
 * 
 * Cerca l'utente nella coda attivi con lo stesso file descriptor e 
 * ne ritorna il puntatore per poterlo usare, successivamente, per
 * la ricerca nella tabella hash.
 * 
 * @return puntatore all'elemento da eliminare, NULL altrimenti.
 */
queueActive *popActive(int arg_fdc, queue *arg_queue);

/**
 * @function insertActive
 * @brief Inserisce un nuovo client alla coda delle associazioni file descriptor-nickname.
 * 
 * @param arg_fdc descrittore della connessione.
 * @param arg_nickname nickname del cliente associato ad arg_fdc.
 * @param arg_queue struttura che contiene le informazioni condivise.
 * 
 * L'inserimento avviene in modo da gestire la coda in ordine descrescente
 * del valore dei file descriptor per ottimizzare i tempi di pop ed insert.
 * 
 * @return 1 se se non ci sono errori altrimenti -1.
 */
int insertActive(int arg_fdc, char* arg_nickname,queue *arg_queue);


/**
 * @function deleteActiveQueue
 * @brief Elimina tutti i nodi dalla lista degli utenti attivi.
 * 
 * @param arg_queue struttura che contiene le informazioni condivise.
 */
void deleteActiveQueue(queue *arg_queue);

/*********FUNZIONI DI UTILITÀ*********************/

/**
 * @function printParams
 * @brief Stampa i parametri attuali del server.
 * 
 * @param params Parametri del server.
 */
static inline void printParams(config params){
	printf("PATH: %s\n", params.UnixPath);
	printf("MAX CONN: %d\n",params.MaxConnections);
	printf("THREAD POOL: %d\n",params.ThreadsInPool); 
	printf("MAX MSG SIZE: %d\n",params.MaxMsgSize);
	printf("MAX FILE SIZE: %d\n",params.MaxFileSize); 
	printf("MAX HIST MSGS: %d\n",params.MaxHistMsgs); 
	printf("DIR NAME: %s\n",params.DirName); 
	printf("STAT FILE NAME: %s\n",params.StatFileName);
}

/**
 * @function initThreadParams
 * @brief Inizializza la struttura dei parametri per i threads
 * 
 * @param params Parametri del server.
 * @param arg_queue struttura che contiene le informazioni condivise.
 * 
 * @return 1 se non ci sono errori altrimenti -1
 */
int initThreadParams(config *params,queue *arg_queue);


/**
 * @function checkParams
 * @brief Controlla la validità dei parametri di configurazione.
 * 
 * @param params Parametri del server.
 * 
 * Se ThreadsInPool > MAX_THREAD gli assegna il valore di 
 * default MAX_THREAD. 
 * Se lo UnixPath > UNIX_PATH_MAX termina ritornando -1.
 * 
 * @return 1 se non ci sono errori altrimenti -1.
 */
int checkParams(config *params);



/**
 * @function maskFunction
 * @brief Maschera i segnali SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGUSR1.
 * 
 * @param set Maschera dei segnali.
 * 
 * @return 1 se non ci sono errori altrimenti -1(settando errno) o 0.
 * 		   
 */
int maskFunction(sigset_t *set);


/**
 * @function socketInit
 * @brief Inizializza il listen socket.
 * 
 * @param arg_sock Address utilizzato nella bind().
 * @param params Parametri del server.
 */
int socketInit(struct sockaddr_un *arg_sock,config *params);

/**
 * @function initOnlineList
 * @brief Inizializza la lista degli utenti attualmente online.
 * 
 * @param arg_queue_active lista degli utenti attivi.
 * @param arg_online_list lista in cui andare ad aggiungere gli utenti attivi.
 * 
 * Copia dentro arg_online_list uno dopo l'altro i nickname degli utenti aggiungendo un padding
 * per farli arrivare tutti a MAX_NAME_LENGTH e terminarli con il carattere terminatore.
 * Esempio: nome1{spazi}\0nome2{spazi}\0.....nomeN{spazi}\0
 * 
 */
void initOnlineList(queueActive *arg_queue_active, char* arg_online_list);

/**
 * @function createSubDir
 * @brief Crea tutte le directory presenti in arg_path.
 * 
 * @param fd_dir path assoluto da dove iniziare la creazione.
 * @param arg_path path contenente le directory da creare.
 * 
 * Crea tutte le directory presenti all'interno di arg_path
 * se non esistono a partire dalla posizione definita dal file
 * descriptor fd_dir.
 * NB : arg_path contiene anche il nome del file.
 * Esempio: 
 * fd_dir = file descriptor di /tmp/chatty.
 * arg_path = ./DATA/chatty.conf1
 * Il nome del file viene "nascosto" e verrà creata la directory
 * DATA se non esistente. Risultato : /tmp/chatty/DATA
 * 
 * @return 1 se non ci sono errori, altrimenti -1.
 * 
 */
int createSubDir(int fd_dir,char *arg_path);
#endif /* CHATTYUTIL_H */
