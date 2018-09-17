/**
 * @file parser.h
 * @brief Contiene definizioni, macro e prototipi per il parsing.
 * Si dichiara che il contenuto di questo file e' in ogni sua parte opera  
 * originale dell'autore.
 */


#ifndef PARSER_H
#define PARSER_H



#define UP "UnixPath"
#define MC "MaxConnections"
#define TnP "ThreadsInPool"
#define MMS "MaxMsgSize"
#define MFS "MaxFileSize"
#define MHM "MaxHistMsgs"
#define DR "DirName"
#define SFN "StatFileName"



/**
 * @struct config
 * @brief Parametri del server.
 * 
 * @var UnixPath path utilizzato per la creazione del socket AF_UNIX.
 * @var MaxConnections numero massimo di connessioni pendenti.
 * @var ThreadsInPool numero di thread nel pool.
 * @var MaxMsgSize dimensione massima di un messaggio testuale (numero di caratteri).
 * @var MaxFileSize dimensione massima di un file accettato dal server (kilobytes).
 * @var MaxHistMsgs numero massimo di messaggi che il server 'ricorda' per ogni client.
 * @var DirName directory dove memorizzare i files da inviare agli utenti.
 * @var StatFileName file nel quale verranno scritte le statistiche del server.
 */
typedef struct _config{
	char *UnixPath;
	int MaxConnections;
	int ThreadsInPool;
	int MaxMsgSize;
	int MaxFileSize;
	int MaxHistMsgs;
	char *DirName;
	char *StatFileName;
}config;



/**
 * @function startParsing
 * @brief Inizia il parsing del file.
 * 
 * @param opzioni Parametri per il parsing.
 * @param num_arg Numero di elementi in opzioni.
 * @param arg_params Parametri del server.
 * 
 * @return 0 se non ci sono errori altrimenti -1
 */
int startParsing(int num_arg,char *opzioni[],config *arg_params);



/**
 * @function deleteConfig
 * @brief Elimina la struttura contenente i parametri.
 * 
 * @param params Parametri del server.
 */
void deleteConfig(config *params);



#endif /*PARSER_H*/
