/**
 * @file parser.c
 * @brief Parser del file di configurazione.
 * Si dichiara che il contenuto di questo file e' in ogni sua parte opera  
 * originale dell'autore.
 * 
 * Prende in input il file di configurazione e
 * ricerca al suo interno i parametri di init 
 * per il server. Se questi non vengono trovati
 * ritorna un errore.
 * Le righe del file di configurazione DEVONO 
 * essere terminate con il carattere '\n'.
 * Gli spazi all'interno dei path verranno cancellati.
 */


#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <parser.h>



/**
 * @function initialize
 * @brief Inizializza i parametri di partenza del server.
 * 
 * @param clear_string Nome del parametro e rispettivo valore
 * @param params Parametri del server.
 * 
 * Tokenizza la stringa passata come parametro.
 * Controlla se è un assegnamento del tipo A=B e
 * controlla se A è uguale ad uno dei parametri del server
 * contenuti in params.
 * 
 * @return 0 se non ci sono errori altrimenti -1.
 */
static int initialize(char *clear_string,config *params){
	/* Puntatore alla stringa generata del token. */
	char *token = NULL;
	/* Lato sinistro dell'assegnamento. */
	char *lhs = NULL;
	/* Lato destro dell'assegnamento. */
	char *rhs = NULL; 
	/* Lunghezza del token. */
	int len = 0; 
	/* Lato dell'assegnamento attuale: 0 = sinistro - 1 = destro. */
	int side = 0; 
	while((token = strtok_r(clear_string,"=",&clear_string)) != NULL){
		len = strlen(token);
		if( side == 0){ 
			if((lhs = strndup(token,len)) == NULL){
				perror("Errore nel token del membro sinistro dell'assegnamento");
				free(token);
				return -1;
			}
			side++;
		}else if ( side == 1){
			if((rhs = strndup(token,len)) == NULL){
				perror("Errore nel token del membro destro dell'assegnamento");
				free(token);
				return -1;
			}
			side++;
		}
	}
	/* Se è un assegnamento del tipo A=B. */
	if(lhs != NULL && rhs != NULL){
		/* Confronto il lato sinistro dell'assegnamento con i miei parametri e assegno. */
		if(strcmp(lhs,UP) == 0){
			if((params->UnixPath = strndup(rhs,strlen(rhs))) == NULL){
				perror("Errore nell'assegnamento di UnixPath");
				free(rhs);
				free(lhs);
				free(token);
				return -1;
			}
		}
		else if(strcmp(lhs,MC) == 0) params->MaxConnections = atoi(rhs);
		else if(strcmp(lhs,TnP) == 0) params->ThreadsInPool = atoi(rhs);
		else if(strcmp(lhs,MMS) == 0) params->MaxMsgSize = atoi(rhs);
		else if(strcmp(lhs,MFS) == 0) params->MaxFileSize = atoi(rhs);
		else if(strcmp(lhs,MHM) == 0) params->MaxHistMsgs = atoi(rhs);
		else if(strcmp(lhs,DR) == 0){
			if((params->DirName = strndup(rhs,strlen(rhs))) == NULL){
				perror("Errore nell'assegnamento di DirName");
				free(rhs);
				free(lhs);
				free(token);
				return -1;
			}
		}
		else if(strcmp(lhs,SFN) == 0){
			if((params->StatFileName = strndup(rhs,strlen(rhs))) == NULL){
				perror("Errore nell'assegnamento di StatFileName");
				free(rhs);
				free(lhs);
				free(token);
				return -1;
			}
		}
		free(lhs);
		free(rhs);
	}else{
		/* Se c'è solo un token vuol dire che non è un assegnamento ma esiste solo il lato sinistro. */
		free(lhs); 
	}
	return 0;
}



/**
 * @function clear
 * @brief Elimina dalla stringa i caratteri non necessari.
 * 
 * @param lineptr Una riga del file di configurazione.
 * @param r dimensione della riga.
 * 
 * Ripulisce la stringa dai seguenti caratteri  '\f','\n','\r','\t','\v',' '.
 * Se c'è un commento questo non viene letto e si ritorna la stringa letta fin ora
 * Se i path e i file contengono degli spazi questi vengono eliminati.
 * 
 * @return Puntatore alla stringa ripulita, NULL altrimenti.
 */
static char *clear(char *lineptr,size_t r){ 
	char *clear_string = calloc(r,sizeof*clear_string);
	if(clear_string == NULL){
		perror("Allocazione della stringa ripulita fallita");
		return NULL;
	}
	int j = 0;
	int i = 0;
	while(lineptr[i] != '\0'){
		/* Sto per leggere un commento che non mi interessa. */
		if(lineptr[i] == '#') return clear_string;
		/*Se il carattere è diverso da:{'\f','\n','\r','\t','\v',' '}. */
		if(isspace(lineptr[i]) == 0){
			clear_string[j] = lineptr[i];
			//printf("VALORE %c VALORE NUOVO %c \n",lineptr[i],clear_string[j]);
			j++;
		}
		i++;
	}
	clear_string[j] = '\0';
	if((clear_string = realloc(clear_string,(strlen(clear_string)+1)*sizeof*clear_string)) == NULL){
		perror("Allocazione della stringa ridimenzionata fallita");
		return NULL;
	}
	return clear_string;
}



/**
 * @function parse
 * @brief Esegue il parsing del file.
 * 
 * Estrae una riga per volta dal file, la ripulisce
 * e inizializza i parametri della struttura passata
 * come parametro.
 * Il file DEVE contenere il carattere '\n' alla fine
 * di ogni riga.
 * La getline(lineptr,n,fp) chiamata con parametri:
 * lineptr = 0 e n = 0 alloca lineptr della dimensione
 * per contenere una riga.
 * 
 * @param fp File di configurazione.
 * @param params Parametri del server.
 * 
 * @return params se non ci sono stati errori altrimenti NULL.
 */
static config *parse(FILE *fp,config *params){
	/* Riga corrente nel file. */
	char *lineptr = NULL;
	/* Lunghezza della riga. */
	size_t n = 0;
	/* Numero di caratteri letti dalla getline(). */
	int r;
	/* Stringa ripulita dai caratteri inutili e/o commenti. */
	char *clear_string = NULL;
	while((r=getline(&lineptr,&n,fp)) != -1){
		if(lineptr[0] != '#' && ((r-1) > 0)){
			if((clear_string = clear(lineptr,r)) == NULL){
				free(lineptr);
				return NULL;
			}
			if(clear_string[0] != '\0'){ /*Se la stringa ritornata è NON vuota*/
				if(initialize(clear_string,params) == -1){
					return NULL;
				}
			}
			free(clear_string);
			//free(lineptr);
			//n=0;
			errno = 0;
		}
	}
	if(r == -1 && errno != 0){
		perror("Impossibile prelevare la riga");
		free(lineptr);
		return NULL;
	}
	free(lineptr);
	return params;
}



/**
 * @function initParams
 * @brief Inizializza i parametri del server a valori di default.
 * 
 * @param params Parametri del server.
 */
static void initParams(config *params){
	params->UnixPath = NULL;
	params->MaxConnections = -1;
	params->ThreadsInPool = -1;
	params->MaxMsgSize = -1;
	params->MaxFileSize = -1;
	params->MaxHistMsgs = -1;
	params->DirName = NULL;
	params->StatFileName = NULL;
}



/* Descrizione presente nell'header essendo una funzione d'interfaccia. */
int startParsing(int num_arg,char *opzioni[],config *arg_params){
	/* File contenente i parametri. */
	FILE* fp;
	/* Opzione ritornata da getopt(). */
	int opt;
	config *params = arg_params;
	while((opt = getopt(num_arg,opzioni,"f:")) != -1){
		switch(opt){
			case 'f':
				if(strcmp(optarg,":") == 0){
					fprintf(stderr,"ERRORE:Nessun argomento passato al parametro -f <nome_file>\n");
					return -1;
				}else{
					/*if(chdir("./DATA") == -1){
						perror("Impossibile cambiare working directory");
						return -1;
					}*/
					if((fp = fopen(optarg,"r")) == NULL){
						perror("Impossibile aprire il file");
						return -1;
					}
					
					printf("CONFIG FILE NAME : %s\n",optarg);
				}
				break;
			default:
				fprintf(stderr,"ERRORE:Parametro errato. Utilizzare -f <nome_file>\n");
				return -1;
		}
	}
	initParams(params);
	if(parse(fp,params) == NULL){
		if(fclose(fp)!= 0) perror("Chiusura del file non riuscita");
		return -1;
	}
	if(params->MaxConnections < 0 || params->ThreadsInPool <= 0 || params->MaxMsgSize <= 0 || \
		params->MaxFileSize <= 0 || params->MaxHistMsgs < 0 || params->UnixPath == NULL || \
			params->DirName == NULL || params->StatFileName == NULL){
		fprintf(stderr,"ERRORE:Parametri di configurazione errati\n");
		if(params->StatFileName != NULL) free(params->StatFileName);
		if(params->DirName != NULL)free(params->DirName);
		if(params->UnixPath != NULL)free(params->UnixPath);
		//free(params);
		if(fclose(fp)!= 0) perror("Chiusura del file non riuscita");
		return -1;
	}
	if(fclose(fp)!= 0){
		perror("Chiusura del file non riuscita");
		return -1;
	}
	return 0;
}



/* Descrizione presente nell'header essendo un metodo d'interfaccia. */
void deleteConfig(config *params){
	free(params->StatFileName);
	free(params->DirName);
	free(params->UnixPath);
}
