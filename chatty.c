/*
 * membox Progetto del corso di LSO 2017
 *
 * Dipartimento di Informatica Università di Pisa
 * Docenti: Prencipe, Torquati
 * 
 */

/**
 * @file chatty.c
 * @brief File principale del server chatterbox
 * Si dichiara che il contenuto di questo file e' in ogni sua parte opera  
 * originale dell'autore.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/socket.h>
#include <stats.h>
#include <connections.h>
#include <sys/mman.h>
#include <chattyutil.h>

/* struttura che memorizza le statistiche del server, struct statistics 
 * e' definita in stats.h.
 */
struct statistics  chattyStats = { 0,0,0,0,0,0,0 };



/* Flag per un segnale di terminazione. */
volatile sig_atomic_t sig_close = 0;

/* Mutex della struttura queue (parametro di master,workers,signal_handler) per i campi :head,tail. */
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER; 
/* Mutex della struttura queue (parametro di master,workers,signal_handler) per i campi :fd_num_max,set, headActive e struttura statistiche {online,register} */
pthread_mutex_t mtx_set = PTHREAD_MUTEX_INITIALIZER;
/* Mutex per la tabella hash. */
pthread_mutex_t mtx_hash[N_MUTEX_HASH] = { PTHREAD_MUTEX_INITIALIZER };
/* Mutex per la struttura delle statistiche : ndelivered, nnotdelivered, nfiledelivered, nfilenotdelivered,nerrors */
pthread_mutex_t mtx_stat = PTHREAD_MUTEX_INITIALIZER;
/* Variabile di condizione per la coda condivisa delle richieste riferita alla mutex mtx. */
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

/**
 * @function signal_handler
 * @brief gestore dei segnali
 * 
 * @param arg struttura di config contenente i path utilizzati
 * 
 * @return 0 se non ci sono errori, -1 altrimenti.
 */
static void *signal_handler(void* arg){
	sigset_t set;
	int sig;
	config *params = (config*) arg;
	char *UnixPath = params->UnixPath;
	char *StatFileName = params->StatFileName;
	FILE *fp = fopen(StatFileName,"a");
	if(maskFunction(&set) == -1) return (void*) -1;
	while(sig_close == 0){
		/* Si mette in attesa di un segnale tra : INT,QUIT,TERM, SIGUSR1, PIPE. */
		if(sigwait(&set,&sig) != 0){
			perror("Failed to sigwait\n");
			fclose(fp);
			return (void*) -1;
		}
		if(sig == SIGINT || sig == SIGQUIT || sig == SIGTERM){
			fprintf(stderr,"ARRIVATO UN SEGNALE DI TERMINAZIONE\n");
			if(unlink(UnixPath)==-1) perror("Errore nel cancellare la socket");
			sig_close = 1;
			pthread_cond_broadcast(&cond);
		}else if(sig == SIGUSR1){
			fprintf(stderr,"***********ARRIVATO SIGUSR1************\n");
			/* Acquista le lock e printa le stats sul file. */
			pthread_mutex_lock(&mtx_set);
			pthread_mutex_lock(&mtx_stat);
			printStats(fp);
			pthread_mutex_unlock(&mtx_stat);
			pthread_mutex_unlock(&mtx_set);
		}
	}
	fclose(fp);
	return (void*) 0;
}

/**
 * @function worker
 * @brief elabora le richieste dei client
 * 
 * @param arg_queue struttura contenente le 
 * 					strutture dati e i parametri di configurazione
 * 
 * @return 0 se non ci sono errori, altrimenti -1
 */
static void *worker(void* arg_queue){
	/* Maschera i segnali di INT,TERM,QUIT,PIPE. */
	sigset_t mask_set;
	maskFunction(&mask_set);
	/* Parametri per il thread. */
	queue *client_queue = (queue*) arg_queue;
	/* Maschera dei bit delle richieste per poterle settare. */
	pthread_mutex_lock(&mtx_set);
	fd_set *set = &client_queue->set;
	pthread_mutex_unlock(&mtx_set);
	/* Valore di ritorno delle read. */
	int r;
	/* File descriptor di un client. */
	int fd_c;
	/* Chiave della tabella hash. */
	unsigned int hashval;
	icl_hash_t *hash = client_queue->hash;
	/* Lunghezza massima del messaggio che si può inviare. */
	int max_msg_len = client_queue->params->MaxMsgSize;
	/* Lunghezza massima del file che si può inviare. */
	unsigned int max_file_len = client_queue->params->MaxFileSize * 1024;
	/* Lunghezza massima della history di un client. */
	int max_hist_len = client_queue->params->MaxHistMsgs;
	/* Valore di ritorno compare. */
	int nick_compare;
	/* Directory dei file ricevuti dai client. */
	DIR *filedir;
	/*File descriptor della directory filedir. */
	int fd_dir;
	/* Messaggio di richiesta di una operazione da parte dell'utente. */
	message_t *msg_in;
	if((msg_in= calloc(1,sizeof(message_t))) == NULL){
		fprintf(stderr,"Impossibile allocare il messaggio generale");
		return (void*) -1;
	}
	/* Header di risposta all'utente. */
	message_hdr_t *hdr_out;
	if((hdr_out = calloc(1,sizeof(message_hdr_t))) == NULL){
		fprintf(stderr,"Impossibile allocare l'header generale");
		free(msg_in);
		return (void*) -1;
	}
	/* Finchè non arriva un segnale di terminazione. */
	while(sig_close == 0){
		pthread_mutex_lock(&mtx);
		/* Fin tanto che la coda è vuota o NON è arrivato un segnale di term. */
		while(client_queue->head == NULL && sig_close == 0){
			pthread_cond_wait(&cond,&mtx);
		}
		if(sig_close == 0){
			/* Preleva la richiesta dalla coda. */
			fd_c = pop(client_queue);
			pthread_mutex_unlock(&mtx);
			/* Apre la directory dei file e ne preleva il file descriptor fd_dir. */
			if((filedir = opendir(client_queue->params->DirName)) == NULL){
				perror("Errore nell'apertura della directory dei file");
				return (void*) -1;
			}
			if((fd_dir = dirfd(filedir)) == -1){
				perror("Impossibile prendere il file descriprot della directory");
				if(closedir(filedir) == -1) perror("Impossibile chiudere la file directory");
				return (void*) -1;
			}
			/* Ripulisce il contenuto da eventuali messaggi precedenti. */
			memset(msg_in,0,sizeof(message_t));
			memset(hdr_out,0,sizeof(message_hdr_t));
			/* Legge la richiesta del client. */
			if((r = readMsg(fd_c,msg_in)) == -1){
				/* Se c'è stato un errore nella lettura. */
				perror("Errore nella lettura del messaggio inviato");
				if(msg_in->data.buf != NULL) free(msg_in->data.buf);
				CLEAREXIT
				if(close(fd_c) == -1) {
					perror("Impossibile chiudere il file descriptor"); 
				}
				return (void*) -1;
			}else if( r == 0){ 
				/* EOF */
				/* Elemento della coda dei client attivi. */
				queueActive *tmp_active_client;
				pthread_mutex_lock(&mtx_set);
				/* Ricerca del nuovo massimo file descriptor, effettua tutto per side-effects. */
				findMax(fd_c,client_queue);
				if((tmp_active_client = popActive(fd_c, client_queue)) != NULL){
					/** 
					 * Se esiste un client con quel file descriptor ed il suo nickname è diverso
					 * dal nickname speciale "" che non fa parte ne degli utenti registrati ne online.
					 */
					nick_compare = strncmp(tmp_active_client->nickname,"",MAX_NAME_LENGTH+1);
					if(nick_compare != 0 ){
						clientReg *tmp_hash_client;
						hashval = ulong_hash_function(tmp_active_client->nickname) % N_BUCKETS;
						pthread_mutex_lock(&mtx_hash[hashval % N_MUTEX_HASH]);
						/* Ricerca il client che è andato offline. */
						if((tmp_hash_client = (clientReg*) icl_hash_find(hash,tmp_active_client->nickname)) != NULL){
							/* Aggiorna lo stato online del client. */
							tmp_hash_client->online = 0;
						}
						pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
						--chattyStats.nonline;
					}
					if(close(fd_c) == -1) {
						pthread_mutex_unlock(&mtx_set);
						CLEAREXIT 
						perror("Impossibile chiudere il file descriptor"); 
						return (void*) -1; 
					}
					fprintf(stderr,"TERMINATO : UTENTE : %s FILE-DESCRIPTOR : %d\n",tmp_active_client->nickname,fd_c);
					/* Se è diverso dal nickname speciale lo cerco nella tabella hash per portarlo offline. */
					/* Elimina il client. */
					free(tmp_active_client);
					tmp_active_client = NULL;
				}
				pthread_mutex_unlock(&mtx_set);
			}else if( r > 0){
				/* È stato letto qualcosa. */
				switch( msg_in->hdr.op ) {
					
					case REGISTER_OP:
						/* Genera la chiave della tabella hash. */
						hashval = ulong_hash_function(msg_in->hdr.sender) % N_BUCKETS;
						fprintf(stderr,"REGISTER : IL NOME DEL CLIENT È: %s   FILE-DESCRIPTOR: %d\n",msg_in->hdr.sender,fd_c);
						/* Acquista e mutex per la coda attivi e per la porzione di tabella hash. */
						pthread_mutex_lock(&mtx_set);
						pthread_mutex_lock(&mtx_hash[hashval % N_MUTEX_HASH]);
						if(icl_hash_find(hash, msg_in->hdr.sender) != NULL || chattyStats.nonline == client_queue->params->MaxConnections){
							/* Nickname già esistente. */
							fprintf(stderr,"REGISTER : GIÀ ESISTENTE O MAX ONLINE. UTENTE: %s   FILE-DESCRIPTOR: %d\n",msg_in->hdr.sender,fd_c);
							/**
							 * Vengono inseriti tutti nella lista anche se l'operazione è fallita per facilitare la loro ricerca
							 * quando termineranno.
							 * Se li aggiungo tutti il costo medio di ricerca è circa O(n/2) mentre se non lo faccio per coloro 
							 * che l'op fallisce il costo è O(n) sempre perché scorrerà tutta la lista dei client attivi. 
							 */
							
							/* Inserisce nickname speciale e file descriptor nella coda attivi. */
							if(insertActive(fd_c,"",client_queue) == -1){
								/* Se l'inserimento fallisce spedisce un messaggio di fallimento. */
								pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								pthread_mutex_unlock(&mtx_set);
								fprintf(stderr,"Impossibile inserire il nuovo utente\n");
								/* Manda OP_FAIL al client. */
								hdr_out->op = OP_FAIL;
								INCRSTAT(chattyStats.nerrors)
								/* SEI ARRIVATO QUI */
								if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
									if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura che c'è stato errore nella coda attivi. ");
								}
								CLEAREXIT
								/* Setta per farlo controllare da un altro worker. */
								LOCKSET(fd_c)								
								return (void*) -1;
							}
							
							
							/* Inserisce il codice relativo all'operazione da mandare. */
							if(chattyStats.nonline == client_queue->params->MaxConnections) hdr_out->op = OP_FAIL;
							else hdr_out->op = OP_NICK_ALREADY;
							if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
								if(errno != EPIPE && errno != EBADF){
									perror("Impossibile scrivere che il nick esiste già");
									CLEAREXIT
									/* Setta per farlo controllare da un altro worker. */
									LOCKSET(fd_c)
									return (void*) -1;
								}
							}
							pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
							pthread_mutex_unlock(&mtx_set);
						}else{ 
							/* Nuovo utente. Nickname da inserire. */
							fprintf(stderr,"REGISTER : DA INSERIRE. UTENTE: %s   FILE-DESCRIPTOR: %d\n",msg_in->hdr.sender,fd_c);
							/* Inizializzo un nuovo elemento da inserire nella tabella hash. */
							clientReg *newClient;
							if((newClient= calloc(1,sizeof(clientReg))) == NULL){
								fprintf(stderr,"Errore nell'allocazione di un nuovo client.\n");
								pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								pthread_mutex_unlock(&mtx_set);
								/* Mando il fallimento dell'operazione. */
								hdr_out->op = OP_FAIL;
								INCRSTAT(chattyStats.nerrors)
								if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
									if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura che c'è stato errore nella tabella hash");
								}
								CLEAREXIT
								/* Setta per farlo controllare da un altro worker. */
								LOCKSET(fd_c)
								return (void*) -1;
							}
							/**
							 * Inizializzo l'elemento da inserire nella tabella hash con nickname
							 * e file descriptor uguali a quelli del mittente.
							 * Viene inserito con valore di online = 0 perché il client dopo un'operazione
							 * di -c NON può specificare alcun'altra operazione diversa da -k (che lo metterà online).
							 * Questo viene fatto per evitare (anche nei test) la sequenza :
							 * -registrazione
							 * -nuovo messaggio
							 * -quit
							 * -nuova connessione dell'utente che doveva ricevere il 'nuovo messaggio'
							 */
							modHashElement(newClient,fd_c,0,msg_in->hdr.sender);
							/* Inserisce nella tabella hash il nuovo utente. */
							if(icl_hash_insert(hash, &(newClient->nickname[0]), newClient) == NULL){
								/* Se fallisce rilascia la mutex. */
								pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								pthread_mutex_unlock(&mtx_set);
								fprintf(stderr,"Impossibile inserire il nuovo utente registrato\n");
								/* Manda OP_FAIL al client. */
								hdr_out->op = OP_FAIL;
								INCRSTAT(chattyStats.nerrors)
								if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
									if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura che c'è stato errore nella tabella hash");

								}
								CLEAREXIT
								free(newClient);
								/* Setta per farlo controllare da un altro worker. */
								LOCKSET(fd_c)
								return (void*) -1;
							}
							/* Inserisce nickname e file descriptor nella coda attivi. */
							if(insertActive(fd_c,"",client_queue) == -1){
								pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								pthread_mutex_unlock(&mtx_set);
								fprintf(stderr,"Impossibile inserire il nuovo utente\n");
								/* Manda OP_FAIL al client. */
								hdr_out->op = OP_FAIL;
								INCRSTAT(chattyStats.nerrors)
								/* SEI ARRIVATO QUI */
								if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
									if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura che c'è stato errore nella coda attivi. ");
								}
								CLEAREXIT
								/* Setta per farlo controllare da un altro worker. */
								LOCKSET(fd_c)								
								return (void*) -1;
							}
							/* Incrementa il numero di utenti registrati. */
							++chattyStats.nusers;
							/**
							 * NON-TRIVIAL : L'utente NON è stato inserito tra gli utenti online
							 * e quindi NON comparirà nella lista dei client attivi. Per ovviare a
							 * questo problema quando si spedisce la lista utenti online il valore di
							 * nonline viene incrementato di 1 **localmente** e il suo nickname viene 
							 * inserito fuori dalla funzione initOnlineList
							 */
							char *buf;
							/* Lunghezza della lista utenti online. */
							int len;
							/* Alloca il buffer per contenere la lista degli utenti online + 1(se stesso). */
							len = (chattyStats.nonline+1)*(MAX_NAME_LENGTH+1);
							if((buf = calloc(len,sizeof(char))) == NULL){
								fprintf(stderr,"ERRORE: Allocazione della lista online fallita.\n");
								pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								pthread_mutex_unlock(&mtx_set);								
								/* Manda OP_FAIL al client. */
								hdr_out->op = OP_FAIL;
								INCRSTAT(chattyStats.nerrors)
								if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
									if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura del fallimento dell'operazione");
								}
								CLEAREXIT
								/* Setta per farlo controllare da un altro worker. */
								LOCKSET(fd_c)	
								return (void*) -1;
							}
							/* Copia il suo nickname all'interno del buffer. */
							strncpy(buf,msg_in->hdr.sender,MAX_NAME_LENGTH);
							/* Aggiunge il padding di spazi. */
							memset(buf+strlen(msg_in->hdr.sender),' ',MAX_NAME_LENGTH -strlen(msg_in->hdr.sender));
							/* Aggiunge il carattere terminatore. */
							buf[MAX_NAME_LENGTH] = '\0';
							/** 
							 * Inizializza il buffer degli utenti online a partire dal
							 * secondo utente (buf+MAX_NAME_LENGTH+1).
							 */
							initOnlineList(client_queue->headActive,buf+MAX_NAME_LENGTH+1);
							
							/* Manda OP_OK al client. */
							hdr_out->op = OP_OK;
							if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
								if(errno != EPIPE && errno != EBADF){
									pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
									pthread_mutex_unlock(&mtx_set);
									CLEAREXIT
									perror("Errore nella scrittura del REG_OP buono");
									/* Setta per farlo controllare da un altro worker. */
									LOCKSET(fd_c)								
									return (void*) -1;
								}
							}
							
							
							/* Manda la stringa degli utenti online al client. */
							message_data_t online_list;
							setData(&online_list,"",buf,len);
							if(sendData(fd_c,&online_list) == -1){
								fprintf(stderr,"Errore nel mandare la lista degli utenti online.\n");
								free(buf);
								CLEAREXIT
								/* Setta per farlo controllare da un altro worker. */
								LOCKSET(fd_c)								
								return (void*) -1;
							}
							/**
							 * E' capitato che arrivasse un messaggio al client connesso al posto della
							 * lista utenti e questo causa il fallimento dell'asserzione nusers>0 nel client
							 * perché il messaggio spedito è più piccolo di MAX_NAME_LENGTH+1 quindi devo
							 * per forza mantenere le lock fino all'ultimo anche se questo, ovviamente causa un
							 * rallentamento.
							 * Test2: pippo manda a pluto il messaggio : "Ti mando un file" ma pluto stava aspettando
							 * la lista utenti.
							 */
							pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
							pthread_mutex_unlock(&mtx_set);
							free(buf);
							buf = NULL;
						}
						break; /* REGISTER_OP */
					case CONNECT_OP:
						/* Calcola la chiave della porzione della tabella hash. */
						hashval = ulong_hash_function(msg_in->hdr.sender) % N_BUCKETS;
						pthread_mutex_lock(&mtx_set);
						pthread_mutex_lock(&mtx_hash[hashval % N_MUTEX_HASH]);
						/* Elemento della tabella hash contenente le informazioni sul mittente. */
						clientReg *tmp_client;
						if(((tmp_client = (clientReg*)icl_hash_find(hash, msg_in->hdr.sender)) != NULL) && (chattyStats.nonline != client_queue->params->MaxConnections)){
							
							
							if(tmp_client->online == 1){ 
								/* Utente registrato già online. Manda operazione fallita. */
								fprintf(stderr," CONN : GIÀ ONLINE. UTENTE : %s\n",msg_in->hdr.sender);
								
								/**
								 * Vengono inseriti tutti nella lista anche se l'operazione è fallita per facilitare la loro ricerca
								 * quando termineranno.
								 * Se li aggiungo tutti il costo medio di ricerca è è O(n/2) mentrse se non lo faccio per coloro 
								 * che l'op fallisce il costo è O(n) in qualsiasi caso.
								 */
								/* Inserisce nickname speciale e file descriptor nella coda attivi. */
								if(insertActive(fd_c,"",client_queue) == -1){
									/* DEVO SPEDIRE IL MESSAGGIO DI ERRORE DI ERRORE AL CLIENT RICORDA*/
									pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
									pthread_mutex_unlock(&mtx_set);
									fprintf(stderr,"Impossibile inserire il nuovo utente\n");
									/* Manda OP_FAIL al client. */
									hdr_out->op = OP_FAIL;
									INCRSTAT(chattyStats.nerrors)
									/* SEI ARRIVATO QUI */
									if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
										if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura che c'è stato errore nella coda attivi. ");
									}
									CLEAREXIT
									/* Setta per farlo controllare da un altro worker. */
									LOCKSET(fd_c)								
									return (void*) -1;
								}
								
								INCRSTAT(chattyStats.nerrors)
								hdr_out->op = OP_FAIL;
								if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
									if(errno != EPIPE && errno != EBADF){
										perror("Errore nella scrittura di fail utente registrato già online");
										CLEAREXIT
										/* Setta per farlo controllare da un altro worker. */
										LOCKSET(fd_c)										
										return (void*) -1;
									}
								}
								pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								pthread_mutex_unlock(&mtx_set);
								
							}else{ 
								/* Utente registrato NON online. Viene connesso. */
								fprintf(stderr,"CONN : NON ONLINE. UTENTE : %s\n",msg_in->hdr.sender);
								tmp_client->online = 1;
								tmp_client->fd_c = fd_c;
								/* Inserisce nickname e file descriptor nella coda attivi. */
								if(insertActive(fd_c,msg_in->hdr.sender,client_queue) == -1){
									pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
									pthread_mutex_unlock(&mtx_set);
									fprintf(stderr,"CONN : FALLITO INSERT ACTIVE. UTENTE : %s\n",msg_in->hdr.sender);
									hdr_out->op = OP_FAIL;
									INCRSTAT(chattyStats.nerrors)
									if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
										if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura che c'è stato errore nella coda attivi. ");
									}
									CLEAREXIT
									/* Setta per farlo controllare da un altro worker. */
									LOCKSET(fd_c)								
									return (void*) -1;
								}
								fprintf(stderr,"CONN : RIUSCITO INSERT ACTIVE. UTENTE : %s\n",msg_in->hdr.sender);
								/* Incrementa il numero di utenti online. */
								++chattyStats.nonline;
								/* Stringa che conterrà gli utenti online. */
								char* buf2;
								/* Lunghezza della lista utenti online. */
								int len2;
								/** 
								 * Viene allocata del numero di utenti
								 * per la loro massima dimensione.
								 */
								len2 = chattyStats.nonline*(MAX_NAME_LENGTH+1);
								if((buf2 = calloc(len2,sizeof(char))) == NULL){
									fprintf(stderr,"CONN : ALLOCAZIONE LISTA FALLITA. UTENTE : %s\n",msg_in->hdr.sender);
									pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
									pthread_mutex_unlock(&mtx_set); 
									/* Manda OP_FAIL al client. */
									hdr_out->op = OP_FAIL;
									INCRSTAT(chattyStats.nerrors)
									if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
										if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura del fallimento dell'operazione");
									}
									CLEAREXIT
									/* Setta per farlo controllare da un altro worker. */
									LOCKSET(fd_c)
									return (void*) -1;
								}
								/* Inizializza la lista degli utenti online. */
								initOnlineList(client_queue->headActive,buf2);
								
								/* Manda operazione riuscita al client. */
								hdr_out->op = OP_OK;
								if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
									if(errno != EPIPE && errno != EBADF){
										pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
										pthread_mutex_unlock(&mtx_set);
										perror("Errore nella scrittura del CONN_OP buono");
										CLEAREXIT
										/* Setta per farlo controllare da un altro worker. */
										LOCKSET(fd_c)								
										return (void*) -1;
									}
								}
								
								/* Manda la stringa degli utenti online al client. */
								message_data_t online_list;
								setData(&online_list,"",buf2,len2);
								if(sendData(fd_c,&online_list) == -1){
									fprintf(stderr,"CONN : ERRORE INVIO LISTA. UTENTE : %s\n",msg_in->hdr.sender);
									free(buf2);
									CLEAREXIT
									/* Setta per farlo controllare da un altro worker. */
									LOCKSET(fd_c)								
									return (void*) -1;
								}
								/**
								 * E' capitato che arrivasse un messaggio al client connesso al posto della
								 * lista utenti e questo causa il fallimento dell'asserzione nusers>0 nel client
								 * perché il messaggio spedito è più piccolo di MAX_NAME_LENGTH+1 quindi devo
								 * per forza mantenere le lock fino all'ultimo anche se questo, ovviamente causa un
								 * rallentamento.
								 * Test2: pippo manda a pluto il messaggio : "Ti mando un file" ma pluto stava aspettando
								 * la lista utenti.
								 */
								pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								pthread_mutex_unlock(&mtx_set);
								free(buf2);
								buf2 = NULL;
								
							}
							
						}else{
							/* Utente NON registrato o massimo utenti raggiunto. Invia fallimento dell'operazione. */
							fprintf(stderr,"CONN : NON REGISTRATO. UTENTE : %s\n",msg_in->hdr.sender);
							/**
							 * Vengono inseriti tutti nella lista anche se l'operazione è fallita per facilitare la loro ricerca
							 * quando termineranno.
							 * Se li aggiungo tutti il costo medio di ricerca è è O(n/2) mentrse se non lo faccio per coloro 
							 * che l'op fallisce il costo è O(n) in ogni caso.
							 */
							
							/* Inserisce nickname speciale e file descriptor nella coda attivi. */
							if(insertActive(fd_c,"",client_queue) == -1){
								pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								pthread_mutex_unlock(&mtx_set);
								fprintf(stderr,"Impossibile inserire il nuovo utente\n");
								/* Manda OP_FAIL al client. */
								hdr_out->op = OP_FAIL;
								INCRSTAT(chattyStats.nerrors)
								/* SEI ARRIVATO QUI */
								if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
									if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura che c'è stato errore nella coda attivi. ");
								}
								CLEAREXIT
								/* Setta per farlo controllare da un altro worker. */
								LOCKSET(fd_c)								
								return (void*) -1;
							}
							
							/* Invia operazione fallita. */
							if(chattyStats.nonline == client_queue->params->MaxConnections) hdr_out->op = OP_FAIL;
							else hdr_out->op = OP_NICK_UNKNOWN;
							if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
								if(errno != EPIPE && errno != EBADF){
									pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
									pthread_mutex_unlock(&mtx_set);
									perror("Errore nella scrittura del REG_OP buono");
									CLEAREXIT
									/* Setta per farlo controllare da un altro worker. */
									LOCKSET(fd_c)
									return (void*) -1;
								}
							}
							pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
							pthread_mutex_unlock(&mtx_set);
						}
						
						break; /* CONNECT_OP */
					case USRLIST_OP:
						/**
						 * Dal client si capisce che un utente può specificare questa operazione sse
						 * prima è stata effettuata una connect (-k) e quindi si assume che l'utente
						 * sia già online altrimenti ci sarebbe già stato un fallimento e l'utente
						 * avrebbe ricevuto un OP_FAIL o un errore dal client stesso.
						 */
						
						/**
						 * NB: Se si elimina questa fprintf aggiungere un semicolon per evitare
						 * che il compilatore si lamenti perché il case comincia con una dichiarazione.
						 */
						fprintf(stderr," USRLIST : IL NOME DEL CLIENT È: %s FILE-DESCRIPTOR: %d\n",msg_in->hdr.sender,fd_c);
						/* Stringa contenente gli utenti online. */
						char* buf3;
						/* Lunghezza della lista utenti online. */
						int len3;
						/* Alloca il buffer per contenere la lista degli utenti online. */
						pthread_mutex_lock(&mtx_set);
						len3 = chattyStats.nonline*(MAX_NAME_LENGTH+1);
						if((buf3 = calloc(len3,sizeof(char))) == NULL){
							fprintf(stderr,"USRLIST : ALLOCAZIONE LISTA FALLITA. UTENTE : %s\n",msg_in->hdr.sender);
							pthread_mutex_unlock(&mtx_set);								
							/* Manda OP_FAIL al client. */
							hdr_out->op = OP_FAIL;
							INCRSTAT(chattyStats.nerrors)
							if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
								if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura del fallimento dell'operazione");
							}
							CLEAREXIT
							/* Setta per farlo controllare da un altro worker. */
							LOCKSET(fd_c)
							return (void*) -1;
						}
						/* Inizializza la lista degli utenti online. */
						initOnlineList(client_queue->headActive,buf3);
						/* Invia operazione riuscita al client. */
						hdr_out->op = OP_OK;
						if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
							if(errno != EPIPE && errno != EBADF){
								//pthread_mutex_unlock(&mtx_set);
								CLEAREXIT
								perror("Errore nella scrittura del REG_OP buono");
								/* Setta per farlo controllare da un altro worker. */
								LOCKSET(fd_c)								
								return (void*) -1;
							}
						}
						
						/* Manda la stringa degli utenti online al client. */
						message_data_t online_list;
						setData(&online_list,"",buf3,len3);
						if(sendData(fd_c,&online_list) == -1){
							fprintf(stderr,"USRLIST : ERRORE INVIO LISTA. UTENTE : %s\n",msg_in->hdr.sender);
							free(buf3);
							CLEAREXIT
							/* Setta per farlo controllare da un altro worker. */
							LOCKSET(fd_c)
							return (void*) -1;
						}
						pthread_mutex_unlock(&mtx_set);
						free(buf3);
						buf3 = NULL;
						break; /* USRLIST_OP */
					case POSTFILE_OP:
					case POSTTXT_OP:
						/**
						 * Dal client si capisce che l'opzione -k deve essere stata già eseguita
						 * e quindi il client è già connesso. I due case vengono gestiti insieme
						 * perché il codice è molto simile.
						 */
						
						/* Il messaggio sarà di almeno 1 elemento perché il client non permette che sia 0. */
						if(strncmp(msg_in->hdr.sender,msg_in->data.hdr.receiver,MAX_NAME_LENGTH+1) != 0){
							if(msg_in->data.hdr.len > (max_msg_len) && msg_in->hdr.op == POSTTXT_OP){
								/* Se il messaggio testuale supera la dimensione massima consentita. */
								fprintf(stderr,"TXT : MESSAGGIO SUPERA IL LIMITE. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n", \
										msg_in->hdr.sender,msg_in->data.hdr.receiver,msg_in->data.buf);
								hdr_out->op = OP_MSG_TOOLONG;
								INCRSTAT(chattyStats.nerrors)
								if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
									if(errno != EPIPE && errno != EBADF){
										perror("Errore nella scrittura del fallimento dell'operazione");
										free(msg_in->data.buf);
										CLEAREXIT
										/* Setta per farlo controllare da un altro worker. */
										LOCKSET(fd_c)
										return (void*) -1;
									}
								}
							}else{
								fprintf(stderr,"TXT : MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n",msg_in->hdr.sender,msg_in->data.hdr.receiver,msg_in->data.buf);
								/* Controlla se destinatario esiste ed è online/offline. */
								clientReg *tmp_hash_client;
								
								/* Calcolo e acquisto la lock relativa al ricevente del messaggio per testare se esiste. */
								hashval = ulong_hash_function(msg_in->data.hdr.receiver) % N_BUCKETS;
								pthread_mutex_lock(&mtx_hash[hashval % N_MUTEX_HASH]);
								if((tmp_hash_client = (clientReg*)icl_hash_find(hash, msg_in->data.hdr.receiver)) != NULL){
									
									pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
									/* Destinatario esistente. */
									if(msg_in->hdr.op == POSTFILE_OP){
										/* Setta l'operazione come file perché verrà utilizzato per mandarlo al destinatario. */
										msg_in->hdr.op = FILE_MESSAGE;
										/* Variabile che conterrà il file e la sua dimensione. */
										message_data_t data;
										memset(&data,0,sizeof(message_data_t));
										if(readData(fd_c,&data) == -1){
											if(data.buf != NULL) free(data.buf);
											free(msg_in->data.buf);
											CLEAREXIT
											LOCKSET(fd_c)
											return (void*) -1;
										}
										if(data.hdr.len > max_file_len){
											/* Se il file è più grande del massimo consentito manda operazione fallita. */
											fprintf(stderr,"MSG : MESSAGGIO SUPERA IL LIMITE. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n", \
													msg_in->hdr.sender,msg_in->data.hdr.receiver,msg_in->data.buf);
											
											hdr_out->op = OP_MSG_TOOLONG;
											INCRSTAT(chattyStats.nerrors)
											if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
												if(errno != EPIPE && errno != EBADF){
													perror("Errore nella scrittura del fallimento dell'operazione");
													/* Gestione errore */
													free(data.buf);
													free(msg_in->data.buf);
													CLEAREXIT
													LOCKSET(fd_c)
													return (void*) -1;
												}
											}
											/* Non esiste altro modo (astuto) di uscire da qui purtroppo. */
											free(data.buf);
											goto TERMINAZIONE;
										}else{
											/* Se la dimensione del file rientra nel massimo. */
											fprintf(stderr,"MSG : MESSAGGIO RIENTRA NEL LIMITE. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n", \
													msg_in->hdr.sender,msg_in->data.hdr.receiver,msg_in->data.buf);
											/* Crea (se non già presenti) tutte le directory contenute nel file ricevuto. */
											if(createSubDir(fd_dir,msg_in->data.buf) == -1){
												/* Se la creazione delle sotto directory fallisce. */
												INCRSTAT(chattyStats.nerrors)
												hdr_out->op = OP_FAIL;
												if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
													if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura del fallimento dell'operazione");
												}
												free(data.buf);
												free(msg_in->data.buf);
												CLEAREXIT
												LOCKSET(fd_c)
												return (void*) -1;
											}
											/* File descriptor dove andare a salvare il file ricevuto. */
											int fd_file;
											/* Reset di errno perché potrebbe essere cambiato con la createSubDir. */
											errno = 0;
											/** 
											 * Vengono utilizzati i flag O_CREAT e O_EXCL combinati in modo che se 
											 * l'operazione di creazione del file non avviene perché già esistente
											 * viene ritornato EEXIST.
											 */
											if((fd_file = openat(fd_dir,msg_in->data.buf,O_RDWR | O_CREAT | O_EXCL,0777)) == -1){
												if(errno != EEXIST){
													perror("Impossibile aprire il file");	
													INCRSTAT(chattyStats.nerrors)
													hdr_out->op = OP_FAIL;
													if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
														if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura del fallimento dell'operazione");
													}
													free(data.buf);
													free(msg_in->data.buf);
													CLEAREXIT
													LOCKSET(fd_c)
													return (void*) -1;
												}
											}
											/* Scrivo ciò che ho ricevuto (il file) sul file aperto. */
											if(errno != EEXIST){
												if(write(fd_file,data.buf,data.hdr.len) == -1){
													perror("Impossibile scrivere nel file");
													if(errno != EBADF && errno != EPIPE){
														INCRSTAT(chattyStats.nerrors)
														hdr_out->op = OP_FAIL;
														if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
															if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura del fallimento dell'operazione");
														}
														if(close(fd_file) == -1) perror("Impossibile chiudere il file");
														free(data.buf);
														free(msg_in->data.buf);
														CLEAREXIT
														LOCKSET(fd_c)
														return (void*) -1;
													}
													
												}
												/* Chiudo il file. */
												if(close(fd_file) == -1){
													perror("Impossibile chiudere il file");
													INCRSTAT(chattyStats.nerrors)
													hdr_out->op = OP_FAIL;
													if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
														if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura del fallimento dell'operazione");
													}
													free(data.buf);
													free(msg_in->data.buf);
													CLEAREXIT
													LOCKSET(fd_c)
													return (void*) -1;
												}
											}
										}
										free(data.buf);
										data.buf = NULL;
									}else if (msg_in->hdr.op == POSTTXT_OP){
										/* Se è un messaggio testuale. */
										msg_in->hdr.op = TXT_MESSAGE;
									}
									/* Riacquisto la lock perché devo controllare se è online o meno. */
									pthread_mutex_lock(&mtx_hash[hashval % N_MUTEX_HASH]);
									if(tmp_hash_client->online == 1){ 
										/* Destinatario esistente online. */
										if(sendRequest(tmp_hash_client->fd_c,msg_in) == -1){
											/* Invio del messaggio al destinatario fallito. */
											fprintf(stderr,"TXT : INVIO MESSAGGIO FALLITO. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n",\
													msg_in->hdr.sender,msg_in->data.hdr.receiver,msg_in->data.buf);
											hdr_out->op = OP_FAIL;
										}else{
											/* Invio riuscito. */
											fprintf(stderr,"TXT : INVIO MESSAGGIO RIUSCITO. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n",\
													msg_in->hdr.sender,msg_in->data.hdr.receiver,msg_in->data.buf);
											/* Incremento le statistiche in base a ciò che invio. */
											if(msg_in->hdr.op == TXT_MESSAGE){
												INCRSTAT(chattyStats.ndelivered)
											}else{
												INCRSTAT(chattyStats.nfiledelivered)
											}
											hdr_out->op = OP_OK;
										}
									}else{
										/* Destinatario esistente NON online. Aggiungo alla history. */
										if(tmp_hash_client->num_hist_msg == max_hist_len){
											/* Se la history è piena. Elimino l'elemento in testa per fare spazio. */
											clientHist *tmp;
											tmp = popHist(tmp_hash_client);
											free(tmp->msg.data.buf);
											free(tmp);
											tmp = NULL;
										}
										/* Se la history NON è piena. */
										if(pushHist(tmp_hash_client,msg_in) == -1){
											/* Se fallisce l'aggiunta alla coda history. */
											fprintf(stderr,"TXT : PUSH HIST FALLITA. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n", \
													msg_in->hdr.sender,msg_in->data.hdr.receiver,msg_in->data.buf);
											hdr_out->op = OP_FAIL;
										}else{
											/* Se l'aggiunta alla coda history va bene. */
											fprintf(stderr,"TXT : PUSH HIST RIUSCITA. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n", \
													msg_in->hdr.sender,msg_in->data.hdr.receiver,msg_in->data.buf);
											/* Incremento le statistiche in base a ciò che invio. */
											if(msg_in->hdr.op == TXT_MESSAGE){
												INCRSTAT(chattyStats.nnotdelivered)
											}else{
												INCRSTAT(chattyStats.nfilenotdelivered)
											}
											hdr_out->op = OP_OK;
										}
									}
									pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
									if(hdr_out->op == OP_FAIL){
											INCRSTAT(chattyStats.nerrors)
									}
									/* Mando il risultato dell'operazione richiesta al mittente. */
									hashval = ulong_hash_function(msg_in->hdr.sender) % N_BUCKETS;
									pthread_mutex_lock(&mtx_hash[hashval % N_MUTEX_HASH]);
									if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
											if(errno != EPIPE && errno != EBADF){
												pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
												free(msg_in->data.buf);
												CLEAREXIT
												perror("Errore nella scrittura del REG_OP buono");
												/* Setta per farlo controllare da un altro worker. */
												LOCKSET(fd_c)	
												return (void*) -1;
											}
									}
									pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								}else{
									/* Il destinatario NON esiste. Mando nickname sconosciuto. */
									fprintf(stderr,"TXT : DESTINATARIO NON ESISTENTE. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n", \
											msg_in->hdr.sender,msg_in->data.hdr.receiver,msg_in->data.buf);
									hdr_out->op = OP_NICK_UNKNOWN;
									if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
										if(errno != EPIPE && errno != EBADF){
											perror("Errore nella scrittura del fallimento dell'operazione");
											pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
											free(msg_in->data.buf);
											CLEAREXIT
											/* Setta per farlo controllare da un altro worker. */
											LOCKSET(fd_c)
											return (void*) -1;
										}
									}
									pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								}
							}
						}else{
							/* Se il destinatario è uguale al mittente mando operazione fallita. */
							hdr_out->op = OP_FAIL;
							INCRSTAT(chattyStats.nerrors)
							if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
								if(errno != EPIPE && errno != EBADF){
									perror("Errore nella scrittura del fallimento dell'operazione");
									free(msg_in->data.buf);
									CLEAREXIT
									/* Setta per farlo controllare da un altro worker. */
									LOCKSET(fd_c)
									return (void*) -1;
								}
							}	
						
						}
			
		TERMINAZIONE: 	free(msg_in->data.buf);
						msg_in->data.buf = NULL;
						break; /* POSTTXT_OP e POSTFILE_OP */
					case GETPREVMSGS_OP:
						/**
						 * Si assume che l'utente sia già registrato e connesso (quindi esiste)
						 * perché il client NON permette di eseguire questa operazione altrimenti.
						 * Quindi non è previsto un caso in cui l'utente NON sia nella tab. hash.
						 */
						/* Mando la risposta alla richiesta. */
						hdr_out->op = OP_OK;
						if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
							if(errno != EPIPE && errno != EBADF){
								perror("Errore nella scrittura del REG_OP buono");
								CLEAREXIT
								/* Setta per farlo controllare da un altro worker. */
								LOCKSET(fd_c)								
								return (void*) -1;
							}
						}
						/* Se l'invio di OP_OK è andato a buon fine */
						clientReg *hash_client;
						/* Calcola la chiave per la porzione della tabella hash. */
						hashval = ulong_hash_function(msg_in->data.hdr.receiver) % N_BUCKETS;
						pthread_mutex_lock(&mtx_hash[hashval % N_MUTEX_HASH]);
						/* Cerca l'utente all'interrno della tabella hash. */
						if((hash_client = (clientReg*)icl_hash_find(hash, msg_in->hdr.sender)) != NULL){
							/* L'utente che richiede l'operazione esiste. */
							/* Manda il numero di messaggi nella history. */
							message_data_t tmp_data;
							memset(&(tmp_data.hdr),0,sizeof(message_data_hdr_t));
							/* La lunghezza è quella di una variabile di tipo size_t. */
							tmp_data.hdr.len = (unsigned int) sizeof(size_t);
							if((tmp_data.buf = calloc(1,sizeof(size_t))) == NULL){
								fprintf(stderr,"GETPREVMSG : CALLOC FALLITA\n");
								pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								/* Invia fallimento dell'operazione. */
								hdr_out->op = OP_FAIL;
								INCRSTAT(chattyStats.nerrors)
								if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
									if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura del fallimento dell'operazione");
									/* Setta per farlo controllare da un altro worker. */
								}
								CLEAREXIT
								LOCKSET(fd_c)
								return (void*) -1;
							}
							/* Inserisce il numero di messaggi nel buffer. */
							*(tmp_data.buf) = hash_client->num_hist_msg;
							/* Manda il numero di messaggi che l'utente deve leggere. */
							if(sendData(fd_c,&tmp_data) == -1){
								fprintf(stderr,"GETPREVMSG : FALLITO INVIO DIMENSIONE HISTORY. UTENTE : %s\n",hash_client->nickname);
								pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								free(tmp_data.buf);
								CLEAREXIT
								LOCKSET(fd_c)
								return (void*) -1;
							}
							fprintf(stderr,"GETPREVMSG : RIUSCITO INVIO DIMENSIONE HISTORY. UTENTE : %s\n",hash_client->nickname);
							free(tmp_data.buf);
							tmp_data.buf = NULL;
							/* Puntatore ad un elemento dei messaggi nella history. */
							clientHist *curr;
							/* Se esiste almeno un messaggio. */
							if(hash_client->num_hist_msg > 0){
								while((curr = popHist(hash_client)) != NULL){
									/* Preleva i messaggi dalla lista. */
									if(curr->msg.hdr.op == TXT_MESSAGE || curr->msg.hdr.op == FILE_MESSAGE){
										/* Manda il messaggio appena prelevato dalla lista. */
										if(sendRequest(fd_c,&(curr->msg)) == -1){
											/* Se l'invio del messaggio fallisce. */
											fprintf(stderr,"GETPREVMSG : FALLITO INVIO MESSAGGIO. UTENTE : %s\n",hash_client->nickname);
											pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
											free(curr->msg.data.buf);
											free(curr);
											CLEAREXIT
											LOCKSET(fd_c)
											return (void*) -1;
										}
										/* Se l'invio non fallisce. */
										fprintf(stderr,"GETPREVMSG : INVIO RIUSCITO. UTENTE : %s\n",hash_client->nickname);
										if(curr->msg.hdr.op == TXT_MESSAGE){
											/*Aggiorna le stats per i messaggi di testo. */
											UPDATESTAT(chattyStats.ndelivered,chattyStats.nnotdelivered)
										}else{
											UPDATESTAT(chattyStats.nfiledelivered,chattyStats.nfilenotdelivered)
										}
									}else{
										/* Se è qualcosa che non dovrebbe essere. Può essere causato da qualche fallimento precedente. */
										fprintf(stderr,"GETPREVMSG : NON È TESTO/FILE. UTENTE : %s\n", hash_client->nickname);
										hdr_out->op = OP_FAIL;
										INCRSTAT(chattyStats.nerrors)
										if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
											if(errno != EPIPE && errno != EBADF){
												perror("Errore nella scrittura del fallimento dell'operazione");
												pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
												free(curr->msg.data.buf);
												free(curr);
												CLEAREXIT
												/* Setta per farlo controllare da un altro worker. */
												LOCKSET(fd_c)
												return (void*) -1;
											}
										}
									}
									free(curr->msg.data.buf);
									free(curr);
									curr = NULL;
								}
							}
							
							
						}
						/**
						 * Rilascio la lock solo quando ho terminato tuttoa l'operazione.
						 * Molto unfair ma considerando che l'history è un numero finito
						 * abbastanza ridotto posso servire la richiesta senza rilasciare
						 * la lock che causerebbe anche dell'overhead per riacquistarla.
						 */
						pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);		
						break; /* GETPREVMSGS_OP */
					case POSTTXTALL_OP:
						/* Il messaggio sarà di almeno 1 elemento perché il client non permette che sia vuoto. */
						if(msg_in->data.hdr.len > (max_msg_len)){
							/* Se supera la dimensione massima consentita. */
							fprintf(stderr,"TXTALL : MESSAGGIO SUPERA IL LIMITE. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n", \
									msg_in->hdr.sender,msg_in->data.hdr.receiver,msg_in->data.buf);
							hdr_out->op = OP_MSG_TOOLONG;
							INCRSTAT(chattyStats.nerrors)
							if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
								if(errno != EPIPE && errno != EBADF){
									perror("Errore nella scrittura del fallimento dell'operazione");
									free(msg_in->data.buf);
									CLEAREXIT
									/* Setta per farlo controllare da un altro worker. */
									LOCKSET(fd_c)
									return (void*) -1;
								}
							}
						}else{
							/* Entry della tabella hash. */
							icl_entry_t * curr_entry;
							clientReg *tmp_txtall;
							/* Error check per fermare la TXTALL. */
							msg_in->hdr.op = TXT_MESSAGE;
							for(int i = 0;i<N_BUCKETS;i++){
								/* Acquista la mutex relativa alla porzione di tabella hash. */
								pthread_mutex_lock(&mtx_hash[i % N_MUTEX_HASH]);
								
								if(hash->buckets[i] != NULL){
									/* Se esiste almeno un elemento in questa entry. */
									curr_entry = hash->buckets[i];
									while(curr_entry  != NULL){
										/* Scorro gli elementi nella entry. */
										tmp_txtall = (clientReg*) curr_entry->data;
										if(strncmp(tmp_txtall->nickname,msg_in->hdr.sender,MAX_NAME_LENGTH+1) != 0){
											if(tmp_txtall->online == 1){ 
												/* Destinatario esistente, online e diverso dal mittente. */
												if(sendRequest(tmp_txtall->fd_c,msg_in) == -1){
													/* Invio del messaggio al destinatario fallito. */
													fprintf(stderr,"TXTALL : INVIO MESSAGGIO FALLITO. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n",\
															msg_in->hdr.sender,tmp_txtall->nickname,msg_in->data.buf);
												}else{
													/* Invio riuscito. */
													fprintf(stderr,"TXTALL : INVIO MESSAGGIO RIUSCITO. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n",\
															msg_in->hdr.sender,tmp_txtall->nickname,msg_in->data.buf);
													INCRSTAT(chattyStats.ndelivered)
												}
											}else{
												/* Destinatario esistente, offline e diverso dal mittente. */
												if(tmp_txtall->num_hist_msg == max_hist_len){
													/* Se la history è piena. */
													clientHist *tmp2;
													tmp2 = popHist(tmp_txtall);
													free(tmp2->msg.data.buf);
													free(tmp2);
													tmp2 = NULL;
												}	
												/* Se la history NON è piena. */
												if(pushHist(tmp_txtall,msg_in) == -1){
													/* Se fallisce l'aggiunta alla coda history. */
													fprintf(stderr,"TXTALL : PUSH HIST FALLITA. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n", \
															msg_in->hdr.sender,tmp_txtall->nickname,msg_in->data.buf);
													
												}else{
													/* Se l'aggiunta alla coda history va bene. */
													fprintf(stderr,"TXTALL : PUSH HIST RIUSCITA. MITTENTE: %s DESTINATARIO : %s MESSAGGIO: %s\n", \
															msg_in->hdr.sender,tmp_txtall->nickname,msg_in->data.buf);
													INCRSTAT(chattyStats.nnotdelivered)
												}
												
											}	
										}
										
										curr_entry = curr_entry->next;
									}
								}
								pthread_mutex_unlock(&mtx_hash[i % N_MUTEX_HASH]);
							
							}
							/** 
							 * Questa operazione può avere solo esito positivo (in generale) anche
							 * se non è stato inviato il messaggio a qualcuno.
							 */
							hdr_out->op = OP_OK;
							if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
								if(errno != EPIPE && errno != EBADF){
									free(msg_in->data.buf);
									CLEAREXIT
									perror("Errore nella scrittura del REG_OP buono");
									/* Setta per farlo controllare da un altro worker. */
									LOCKSET(fd_c)	
									return (void*) -1;
								}
							}
						}
						free(msg_in->data.buf);
						msg_in->data.buf = NULL;
						break; /* POSTTXTALL_OP */
					case GETFILE_OP:
						/** 
						 * Il semicolon serve a non far lamentare il compilatore perché
						 * il case comincia con una dichiarazione e non potrebbe, quindi
						 * gli do una istruzione vuota.
						 */
						;
						/* Informazioni sul file da inviare. */
						struct stat info;
						if(fstatat(fd_dir,msg_in->data.buf,&info,0) == -1){
							/* Se il file non esiste o qualsiasi altro errore. */
							perror("Impossibile repirire informazioni sul file");
							hdr_out->op = OP_NO_SUCH_FILE;
							if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
								if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura del fallimento dell'operazione");
								free(msg_in->data.buf);
								CLEAREXIT
								LOCKSET(fd_c)
								return (void*) -1;
							}
							free(msg_in->data.buf);	
							msg_in->data.buf = NULL;
						}else{
							/* Fil descriptor del file da inviare. */
							int fd_open;
							if((fd_open = openat(fd_dir,msg_in->data.buf,O_RDONLY,0777)) == -1){
								perror("Impossibile aprire il file");
								hdr_out->op = OP_FAIL;
								INCRSTAT(chattyStats.nerrors)
								if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
									if(errno != EPIPE && errno!= EBADF){
										perror("Errore nella scrittura del fallimento dell'operazione");
										free(msg_in->data.buf);
										return (void*) -1;
									}
								}
								free(msg_in->data.buf);
								msg_in->data.buf = NULL;
							}else{
								/* Se il file è stato aperto correttamente inizializzo il messaggio da inviare. */
								message_t msg_out;
								memset(&msg_out,0,sizeof(message_t));
								/* Inizializzo la lunghezza del file, il nome del destinatario e mittente che non servono. */
								msg_out.data.hdr.len = info.st_size;
								strncpy(msg_out.hdr.sender,"",MAX_NAME_LENGTH+1);
								strncpy(msg_out.data.hdr.receiver,"",MAX_NAME_LENGTH+1);
								/* Mappo un'area di memoria privata per contenere il file. */
								msg_out.data.buf = mmap(NULL,info.st_size,PROT_READ,MAP_PRIVATE,fd_open,0);
								if(msg_out.data.buf == MAP_FAILED){
									/* Se la map è fallita. */
									perror("Impossibile leggere il file");
									hdr_out->op = OP_FAIL;
									INCRSTAT(chattyStats.nerrors)
									if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
										if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura del fallimento dell'operazione");
									}
									free(msg_in->data.buf);
									if(close(fd_open) == -1) perror("Chiusura del file fallita");
									CLEAREXIT
									LOCKSET(fd_c)
									return (void*) -1;
								}
								/* Aggiorno le statistiche e mando operazione riuscita al client. */
								fprintf(stderr,"GETFILE: RIUSCITA. UTENTE %s",msg_in->hdr.sender);
								UPDATESTAT(chattyStats.nfiledelivered,chattyStats.nfilenotdelivered)
								msg_out.hdr.op = OP_OK;
								if(sendRequest(fd_c,&msg_out) == -1){
									perror("Errore nell'invio del file"); 
									free(msg_in->data.buf);
									munmap(msg_out.data.buf,info.st_size);
									CLEAREXIT
									LOCKSET(fd_c)
									return (void*) -1;
								}
								free(msg_in->data.buf);
								msg_in->data.buf = NULL;
								if(munmap(msg_out.data.buf,info.st_size) == -1){
									perror("Errore nella munmap");
									CLEAREXIT
									LOCKSET(fd_c)
									return (void*) -1;
								}
								if(close(fd_open) == -1){
									perror("Chiusura del file fallita");
									CLEAREXIT
									LOCKSET(fd_c)
									return (void*) -1;
								}
							}	
						}
						break; /* GETFILE_OP */
					case DISCONNECT_OP:
					case UNREGISTER_OP:
						/**
						 * Ricerco il client tra gli attivi e setto il suo nickname
						 * al nickname speciale "" in modo da eliminarlo solo successivamente
						 * quando mi chiudurà la socket e, se è un'operazione di unregister, elimino 
						 * l'elemento e la history nella tabella hash corrispondente a chi ha inviato l'operazione.
						 */
						pthread_mutex_lock(&mtx_set);
						/* Utente da de registrare. */
						queueActive *tmp_search_active;
						if((tmp_search_active = searchActive(fd_c,client_queue)) != NULL){
							if(strncmp(tmp_search_active->nickname,msg_in->hdr.sender,MAX_NAME_LENGTH+1)==0){
								/* L'utente che richiede l'operazione è lo stesso che è connesso. */
								fprintf(stderr,"UNREG : COMPARE RIUSCITA. NOME: %s\n",msg_in->hdr.sender);
								/* Copio il nickname speciale. */
								strncpy(tmp_search_active->nickname,"",MAX_NAME_LENGTH+1);
								/* Decremento il numero di utenti online. */
								--chattyStats.nonline;
								if(msg_in->hdr.op == UNREGISTER_OP){
									/**
									 * Se è un'operazione di unregister devo decr. gli utenti online ed
									 * eliminarlo dalla tabella hash.
									 */
									--chattyStats.nusers;
									/* Acquisto la mutex relativa a quella porzione di tabella hash. */
									hashval = ulong_hash_function(msg_in->hdr.sender) % N_BUCKETS;
									pthread_mutex_lock(&mtx_hash[hashval % N_MUTEX_HASH]);
									if(icl_hash_delete(hash,&(msg_in->hdr.sender[0]),NULL,freeHashData) == 0){
										/* Delete riuscita. */
										hdr_out->op = OP_OK;
									}else{
										/* Delete fallita. */
										INCRSTAT(chattyStats.nerrors)
										hdr_out->op = OP_FAIL;
									}
									pthread_mutex_unlock(&mtx_hash[hashval % N_MUTEX_HASH]);
								}
							}else{
								/* L'utente che richiede l'operazione è diverso da chi è connesso. */
								fprintf(stderr,"UNREG : COMPARE FALLITA. NOME: %s\n",msg_in->hdr.sender);
								INCRSTAT(chattyStats.nerrors)
								hdr_out->op = OP_FAIL;
							}
						}else{
							/* Non esiste tra gli utenti attivi. */
							fprintf(stderr,"UNREG : NON ESISTE. NOME: %s\n",msg_in->hdr.sender);
							hdr_out->op = OP_NICK_UNKNOWN;
						}
						/* Invio il risultato della sua operazione. */
						if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
							if(errno != EPIPE && errno != EBADF){
								perror("Errore nella scrittura del fallimento dell'operazione");
								pthread_mutex_unlock(&mtx_set);
								CLEAREXIT
								LOCKSET(fd_c)
								return (void*) -1;
							}
						}
						pthread_mutex_unlock(&mtx_set);
						break; /* UNREGISTER_OP */
						
					default:
						/* Non è tra le operazioni consentite. Manda un OP_FAIL. */
						fprintf(stderr,"DEFAULT CASE : UTENTE %s OP: %d BUF: %s REC : %s LEN: %d\n",msg_in->hdr.sender,msg_in->hdr.op,msg_in->data.buf,msg_in->data.hdr.receiver,msg_in->data.hdr.len);
						pthread_mutex_lock(&mtx_set);
						/* Inserisco nickname speciale e file descriptor nella coda attivi. */
						if(insertActive(fd_c,"",client_queue) == -1){
							/* Se l'operazione di inserimento fallisce. */
							pthread_mutex_unlock(&mtx_set);
							fprintf(stderr,"Impossibile inserire il nuovo utente\n");
							/* Mando operazione fallita al client. */
							hdr_out->op = OP_FAIL;
							INCRSTAT(chattyStats.nerrors)
							if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
								if(errno != EPIPE && errno != EBADF) perror("Errore nella scrittura che c'è stato errore nella coda attivi. ");
							}
							if(msg_in->data.buf != NULL) free(msg_in->data.buf);
							CLEAREXIT
							/* Setta per farlo controllare da un altro worker. */
							LOCKSET(fd_c)								
							return (void*) -1;
						}
						/* Mando operazione fallita al client. */
						hdr_out->op = OP_FAIL;
						INCRSTAT(chattyStats.nerrors)
						if(write(fd_c,hdr_out,sizeof(message_hdr_t)) == -1){
							if(errno != EPIPE && errno != EBADF){
								perror("Errore nella scrittura del fallimento dell'operazione");
								if(msg_in->data.buf != NULL) free(msg_in->data.buf);
								CLEAREXIT
								/* Setta per farlo controllare da un altro worker. */
								LOCKSET(fd_c)
								return (void*) -1;
							}
						}
						pthread_mutex_unlock(&mtx_set);
						if(msg_in->data.buf != NULL) free(msg_in->data.buf);
						break; /* DEFAULT */
				}
				/* Setta il bit per farlo controllare alla select dopo che il worker ha finito l'operazione. */
				LOCKSET(fd_c)
			}
			/* Chiude il file descriptor della directory contenente i file. */
			if(closedir(filedir) == -1){
				perror("Impossibile chiudere la directory");
				free(msg_in);
				free(hdr_out);
			}
		}else{
			/* Se è arrivato un segnale di terminazione. */
			pthread_mutex_unlock(&mtx);
		}
	}
	free(msg_in);
	free(hdr_out);
	printf("WORKER TERMINATO\n");
	return (void*) 0;
}


/**
 * @function master
 * @brief gestisce nuove connessioni e richieste di operazioni degli utenti
 * 
 * @param arg_queue struttura contenente le 
 * 					strutture dati e i parametri di configurazione.
 * 
 * @return 0 se non ci sono errori, altrimenti -1.
 */
static void *master(void* arg_queue){
	/* Maschera per i segnali. */
	sigset_t mask_set;
	if(maskFunction(&mask_set) == -1) return (void*) -1;
	/* Struttura timer per la select. */
	struct timeval tv;
	/* Address del socket. */
	struct sockaddr_un sa;
	/* File descriptor di un nuovo utente. */
	int fd_c = 0;
	/* File descriptor delle richieste di utenti già connessi. */
	int fd;
	/* Maschera di bit per i clienti attivi. */
	fd_set rdset;
	/* Struttura condivisa fra worker/master. */
	queue *client_queue = (queue*) arg_queue;
	/* Maschera di bit per i client attivi. */
	fd_set *set = &client_queue->set;
	/* Maschera di bit per mantenere il massimo dei client. */
	fd_set *set_max = &client_queue->set_max;
	/* File descriptor del socket in ascolto. */
	int fd_skt = 0;
	if((fd_skt = socketInit(&sa,client_queue->params)) == -1) return (void*) -1;
	/* Inizializzo il massimo file descriptor a quello in ascolto. */
	client_queue->fd_num_max = fd_skt;
	int fd_num_max = fd_skt;
	/* Inizializza ad 1 il bit del socket in ascolto. */
	FD_SET(fd_skt,set);
	FD_SET(fd_skt,set_max);
	while(sig_close == 0){
		/* Setta il timer a 400 ms ad ogni iterazione. */
		tv.tv_sec = 0;
		tv.tv_usec = 400*1000; 
		pthread_mutex_lock(&mtx_set);
		/* Aggiorna il set su cui andare ad eseguire la select. */
		rdset = *set;
		/* Aggiorna ad ogni giro il massimo file descriptor causa terminazioni. */
		fd_num_max = client_queue->fd_num_max;
		pthread_mutex_unlock(&mtx_set);
		if((select(fd_num_max+1,&rdset, NULL, NULL, &tv)) == -1){
			if(errno != EINTR){
				/* Se non è un segnale. */
				perror("Errore nell'esecuzione della select");
				return (void*) -1;
			}
		}
		if(sig_close == 0){ 
			/* Se non è arrivato un segnale di terminazione. */
			/* Per ogni file descriptor in rdset controlla se è settato. */
			for(fd=0;fd<=fd_num_max;fd++){
				if(FD_ISSET(fd,&rdset)){
					if(fd == fd_skt){ 
						/* Se è il listener socket accetta una nuova connessione. */
						if((fd_c = accept(fd_skt,NULL,0)) == -1){
							perror("Impossibile accettare la connessione");
							break;
						}
						fprintf(stderr,"\t\t\tMASTER : NUOVO CLIENT. FILE DESCRIPTOR : %d\n",fd_c);
						/* Setta il bit per farlo controllare alla select. */
						pthread_mutex_lock(&mtx_set);
						/* Setta il file descriptor come attivo nel set e nel set_max. */
						FD_SET(fd_c,set);
						FD_SET(fd_c,set_max);
						/* Aggiorna il massimo per side-effect nel buffer condiviso. */
						if(fd_c > client_queue->fd_num_max ){
							client_queue->fd_num_max = fd_c;
						}
						pthread_mutex_unlock(&mtx_set);
					}else{
						fprintf(stderr,"\t\t\tMASTER : NUOVO MESSAGGIO. FILE DESCRIPTOR : %d\n",fd);
						pthread_mutex_lock(&mtx_set);
						/* Resetto il bit del file descriptor per non controllarlo nella select. */
						FD_CLR(fd,set);
						pthread_mutex_unlock(&mtx_set);
						pthread_mutex_lock(&mtx);
						/* Pusha sulla coda delle richieste per farlo gestire da un worker. */
						if(push(fd,client_queue) == -1) return (void*) -1;
						pthread_cond_signal(&cond);
						pthread_mutex_unlock(&mtx);
					}
				}
			}
		}
	}
	printf("MASTER TERMINATO\n");
	CLOSE(fd_skt,"Impossibile chiudere il file descriptor listener")
	return (void*) 0;
}



int main(int argc, char *argv[]) {
	/* Se gli argomenti sono meno o più di quelli che aspetta. */
	if(argc != 3){
		usage(argv[0]);
		return -1;
	}
	/* Maschera per i segnali. */
	sigset_t mask_set;
	/* Thread per la gestione dei segnali. */
	pthread_t handler_thread;
	/* Thread worker per l'interazione con i client. */
	pthread_t *worker_thread;
	/* Thread master per la gestione di nuove connessioni/operazioni. */
	pthread_t master_thread;
	/* Configurazione del server. */
	config params;
	/* Struttura da passare ai thread master/workers. */
	queue node;
	/* Maschera i segnali. */
	if(maskFunction(&mask_set) == -1) return -1;
	/* Inizia il parsing del file. */
	if((startParsing(argc,argv,&params)) == -1) return -1;
	/* Controlla alcune proprietà dei parametri. */
	if(checkParams(&params) == -1) return -1;
	printParams(params);
	/* Creazione thread per la gestione dei segnali. */
	NOTZERO(pthread_create(&handler_thread,NULL,signal_handler,&params),\
				"Impossibile creare thread gestore segnali")
	/* Inizializza la struttura per master e workers. */
	if(initThreadParams(&params,&node) == -1) return -1;

	/* Alloca ThreadsInPool workers. */
	CALLOC(worker_thread,params.ThreadsInPool,pthread_t,"Impossibile allocare i worker")
	/* Crea i worker thread. */
	for(int i = 0;i<params.ThreadsInPool;i++){
		NOTZERO(pthread_create(&worker_thread[i],NULL,worker,&node),"Impossibile creare thread worker")
		printf("HO CREATO %d THREAD\n",i);
	}
	/* Crea il master thread. */
	NOTZERO(pthread_create(&master_thread,NULL,master,&node),"Impossibile creare thread master")
	/* Valori di ritorno per i thread. */
	int master_status;
	int handler_status;
	int worker_status[params.ThreadsInPool];
	int join_error;
	
	if((join_error = pthread_join(master_thread,(void**)&master_status))!=0){
		/* Se fallisce genero un SIGTERM per far terminare tutto. */
		errno = join_error;
		perror("Fallito join master");
		kill(getpid(),SIGTERM);
	}
	fprintf(stderr," Ritorno master %d\n",master_status);
	for(int i = 0;i<params.ThreadsInPool;i++){
		if((join_error = pthread_join(worker_thread[i],(void**)&worker_status[i]))!= 0){
			/* Se fallisce genero un SIGTERM per far terminare tutto. */
			errno = join_error;
			perror("Fallito join worker");
			kill(getpid(),SIGTERM);
		}
		fprintf(stderr," Ritorno thread %d : %d\n",i,worker_status[i]);
	}
	if((join_error = pthread_join(handler_thread,(void**)&handler_status)) != 0){
		errno = join_error;
		perror("Fallito join signal handler");
		/* Faccio il suo lavoro per far terminare tutti. */
		sig_close = 1;
	}
	printf(" Ritorno handler %d\n",handler_status);
	/* Cancello la tabella hash, i worker, la configurazione e la lista delle richieste e utenti attivi. */
	if(icl_hash_destroy(node.hash,NULL,freeHashData)==-1) perror("Errore nella cancellazione della tabella hash");
	free(worker_thread);
	deleteConfig(&params);
	deleteActiveQueue(&node);
	deleteQueue(&node);
    return 0;
}
