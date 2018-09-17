#!/bin/bash

# @file chatty.c
# @brief File principale del server chatterbox
# Si dichiara che il contenuto di questo file e' in ogni sua parte opera  
# originale dell'autore.

# Se non ci sono argomenti o sono più di tre o uno tra loro è help
if [[ $# == 0 || $# > 3 || "$1" == "-help" || "$2" == "-help" || "$3" == "-help" ]]; then
	echo "usa $0 config_path t [-help]"
	echo "t = 0 stampa gli elementi contenuti in config_path"
	echo "t > 0 elimina file e directory in config_path più vecchi di t minuti"
	exit 1
fi


# Se il parametro t è negativo
if (("$2" < "0")); then
	echo "t deve essere >=0."
	echo "t = 0 stampa gli elementi contenuti in config_path"
	echo "t > 0 elimina file e directory in config_path più vecchi di t minuti"
	exit 1
fi


# 1. Elimino per prima tutte le righe che iniziano per spazi, tab, newline, formfeed, ritorno a capo e commenti.
# 2. Elimino dal file tutti gli spazi, tab, formfeed e ritorno a capo. (il file, se contiene spazi, viene preso senza essi come fa anche il parser.).
# 3. Splitto con delimitatore # e prendo solo il primo campo (potrei avere : DirName=/mio/path #altrecosequi.
# 4. Prendo solo ciò che matcha la stringa DirName=[alfanumerici e punteggiatura].
# 5. Prendo solo il path che c'è dopo l'uguaglianza.
path=$(grep -v '^[[:space:]]*#' $1 | tr -d ' \t\r\f' | cut -d \# -f 1 | grep -o 'DirName=[[:graph:]]*' | cut -d '=' -f 2)
# $2 contiene il valore in input t
# Controllo se path è una directory
if [ -d "$path" ]; then
	if (( "$2" > "0" )); then
		# Elimino i file e tutte le directory in config_path più vecchie (ultima modifica al contenuto maggiore)di t minuti
		find "$path" -mindepth 1 -mmin "+$2" -delete
	else
		# Stampo sullo stdout i nomi delle directory e il loro contenuto.
		ls -R "$path"
	fi
else
	echo "La directory $path non esiste."
	exit 1
fi
