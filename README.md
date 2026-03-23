# PACMAN

PACMAN é um projeto desenvolvido para a cadeira de Sistema Operativos. O objetivo é criar o jogo PACMAN utilizando interação cliente-servidor com paralelização usando várias threads para aceleração do jogo.

O projeto consite em:

    - Desenvolver um servidor que suporte vários jogos em paralelo iniciados por múltiplos processos clientes ligados ao servidor através de named pipes

    - Desenvolver um signal handler que gere um log para ficheiro que descreva o estado dos jogos atualmente existentes no servidor

Tecnologias:

    - Em linguagem C
    - Conceitos: Threads, Processos, FIFOs, Sinais, Semáforos, Trincos

Como compilar e correr:

    1- Clonar o repositório
    2- Abrir um terminal para o servidor (na diretoria servidor) e X's terminais para clientes (na diretoria cliente)
    3- Em ambos os terminais escrever "make clean all"
    4- No terminal do servidor (na diretoria servidor):
        Lançar na forma (levels_dir     max_games      nome_do_FIFO_de_registo)
            Ex: ./bin/Pacmanist ./teste 1 /tmp/pipe.pipe
    5- No terminal dos clientes (na diretoria cliente): 
        Lançar na forma (id_do_cliente      nome_do_FIFO_de_registo       ficheiro_pacman) 
        #Coloque ficheiro_pacman se não quiser controlar manualmente, e que seja pré definido 
            Ex: ./bin/client 2 /tmp/pipe.pipe 
    






