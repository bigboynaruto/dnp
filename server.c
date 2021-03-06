#include <unistd.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdarg.h>

#include "data.h"
#include "list.h"
#include "stats.h"
#include "termcolors.h"
#include "util.h"
#include "pokegen.h"
#include "serial.h"

#define PROTOPORT         27428
#define QLEN              10
#define MAXRCVLEN 200

#define send_stuff(...) _send_stuff(__VA_ARGS__, NULL)

void _send_stuff(int tsd, ...);

void *conn_handler(void *parm);

void chat(int, char *);
void bc_a_r(struct pk_attack_result);
void send_status(int);
void serv_attack(int, struct action *);
int add_player(char *, int);
bool server_full(void);
void send_class(void *, void *);
void send_skill(void *, void *);

struct player {
	char *name;
	int tsd;
	bool is_online;
	struct pkmn *character;
	struct action *attack;
};


static int sd;
static uint8_t global_level;
static struct player players[2];

static bool kaput = false;

static pthread_mutex_t mut;

void chat(int player, char *msg) {
	char *chat_cl = format_str(C_GREEN, C_FG);
	char *def_cl = format_str(C_DEFAULT, C_FG);
	char *name = players[player].name;
	for (int i = 0; i < 2; ++i)
		if (players[i].tsd)
			send_stuff(players[i].tsd, chat_cl, "\n", name, ": ", msg, "\n", def_cl);
}

#define broadcast(...) for (int __i = 0; __i < 2; ++__i)\
	if (players[__i].tsd)\
		send_stuff(players[__i].tsd, __VA_ARGS__)

void bc_a_r(struct pk_attack_result r) {
	char buf[16], buf1[64];
	sprintf(buf, "%u", r.dhp);
	sprintf(buf1, "%d %d %d %d %d %d\n", r.dattrs.STR, r.dattrs.DEF, r.dattrs.CON,
		r.dattrs.MAG, r.dattrs.DEX, r.dattrs.LCK);
	broadcast("Action: ", r.action->name,
		". Target: ",r.target == TARGET_SELF ? "self" : "opponent",
		". Outcome: ", ser_out(r.outcome),
		". HP Change: ", buf,
		". Attrs change: ", buf1);
}

void send_status(int n) {
	int tsd = players[n].tsd;
	struct pkmn *pk = players[n].character;
	char buf[64], buf1[64], buf2[64];
	char *cul = format_str(C_UL, C_FMT_ON),
		*culo = format_str(C_UL, C_FMT_OFF),
		*cb = format_str(C_BOLD, C_FMT_ON),
		*cbo = format_str(C_BOLD, C_FMT_OFF),
		*cd = format_str(C_DEFAULT, C_FG);
	struct attrs at = pk->attrs;
	sprintf(buf, "%u", pk->lvl);
	sprintf(buf2, "%d STR, %d DEF, %d CON, %d MAG, %d DEX, %d LCK",
		at.STR, at.DEF, at.CON, at.MAG, at.DEX, at.LCK);
	sprintf(buf1, "%u/%u", pk->hp, pk->attrs.CON);
	send_stuff(tsd, "Class: ", cul, pk->cls->name, culo, ", Specialization: ", ser_spec(pk->cls->spec),
		", Level: ", buf, ", HP: ", buf1, ", ", cb, pk->alive ? "ALIVE" : "DEAD", cbo,
		", Attributes: ", buf2, "\n", cd);
}

void serv_attack(int n, struct action *a) {
	char *cl_err = format_str(C_RED, C_FG);
	char *cl_inf = format_str(C_GREEN, C_FG);
	char *cl_def = format_str(C_DEFAULT, C_FG);
	if (!players[!n].character) {
		send_stuff(players[n].tsd, cl_err, "Your opponent is not ready or present yet\n", cl_def);
		return;
	}
	pthread_mutex_lock(&mut);
	players[n].attack = a;
	send_stuff(players[n].tsd, cl_inf, "Your attack was submitted\n", cl_def);
	if (players[!n].character && !players[!n].is_online) {
		list_t *l = list_random_sample(players[!n].character->skills, 1);
		players[!n].attack = l->data;
		free(l);
	}
	if (players[0].attack && players[1].attack) {
		struct attack_result res = attack(players[0].character, players[1].character,
			players[0].attack, players[1].attack);
		broadcast("\nFirst attacker: Player ", res.order & 1 ? "1" : "2", "\n");
		broadcast("Attack 1:");
		bc_a_r(res.p1);
		if (res.order & 1 << 1) {
			broadcast("Attack 2:");
			bc_a_r(res.p2);
		}
		send_status(0);
		send_status(1);
		players[0].attack = players[1].attack = NULL;
	}
	if (!(players[0].character->alive && players[1].character->alive)) {
		kaput = true;
		broadcast(cl_inf, players[0].name, " has ",
			players[0].character->alive ? "won" : "lost", "\n");
		broadcast(cl_inf, players[1].name, " has ",
			players[1].character->alive ? "won" : "lost", "\n");
		close(players[0].tsd);
		close(players[1].tsd);
		close(sd);
	}
	pthread_mutex_unlock(&mut);
}

int add_player(char *name, int tsd) {
	for (int i = 0; i < 2; ++i) {
		if (players[i].name == NULL ||
			(!strcmp(name, players[i].name) && !players[i].is_online)) {
			players[i].name = name;
			players[i].is_online = true;
			players[i].tsd = tsd;
			return i + 1;
		}
		if (!strcmp(name, players[i].name))
			return 0;
	}
	return 0;
}

bool server_full() {
	return players[0].name && players[1].name &&
		players[0].is_online && players[1].is_online;
}

int main(int argc, char *argv[]) {
	struct protoent *ptrp;
	struct sockaddr_in sad;
	struct sockaddr_in cad;
	int sd2;
	int port;
	socklen_t alen;
	pthread_t tid;

	srand(time(NULL));
	init_game_data();
	global_level = rand() % 80 + 1;

	pthread_mutex_init(&mut, NULL);
	memset((char *)&sad, 0, sizeof(sad));
	sad.sin_family = AF_INET;
	sad.sin_addr.s_addr = INADDR_ANY;

	if (argc > 1)
		port = atoi(argv[1]);
	else
		port = PROTOPORT;

	if (port > 0)
		sad.sin_port = htons((u_short) port);
	else {
		fprintf(stderr, "bad port number %s/n", argv[1]);
		exit(EXIT_FAILURE);
	}

	if (!(ptrp = getprotobyname("tcp"))) {
		fprintf(stderr, "cannot map \"tcp\" to protocol number");
		exit(EXIT_FAILURE);
	}

	sd = socket(PF_INET, SOCK_STREAM, ptrp->p_proto);
	if (sd < 0) {
		perror("failed to create a socket");
		exit(EXIT_FAILURE);
	}

	if (bind(sd, (struct sockaddr *)&sad, sizeof(sad)) < 0) {
		perror("failed to bind to socket");
		exit(EXIT_FAILURE);
	}

	if (listen(sd, QLEN) < 0) {
		perror("listen failed");
		exit(EXIT_FAILURE);
	}

	alen = sizeof(cad);

	fprintf(stderr, "Server up and running.\n");
	while (1) {
		if ((sd2 = accept(sd, (struct sockaddr *)&cad, &alen)) < 0) {
			fprintf(stderr, "accept failed\n");
			exit(EXIT_FAILURE);
		}
		if (kaput)
			break;
		pthread_create(&tid, NULL, conn_handler, &sd2);
	}

}

void _send_stuff(int tsd, ...) {
	char *c;
	char buf[2048] = {'\0'};
	va_list argp;
	va_start(argp, tsd);
	while ((c = va_arg(argp, char *)))
		strcat(buf, c);
		//send(tsd, c, strlen(c), 0);
	send(tsd, buf, strlen(buf), 0);
}

void send_class(void *cls, void *p) {
	send_stuff(*((int *)p), ((struct pk_class *)cls)->name, ", ");
}


void send_skill(void *c, void *p) {
	struct action *a = c;
	char buf[128];
	char *cul = format_str(C_UL, C_FMT_ON),
		*culo = format_str(C_UL, C_FMT_OFF),
		*cb = format_str(C_BOLD, C_FMT_ON),
		*cbo = format_str(C_BOLD, C_FMT_OFF);
	sprintf(buf, "%u", a->data.dc_mod);
	send_stuff(*((int *)p), cul, cb, a->name,cbo,  cul, " of type ",
		cul, cb, ser_actt(a->type), cbo, culo,
		" targeting ", cul, a == TARGET_SELF ? "self" : "opponent", culo,
		" with the difficulty class of ", buf);
	switch (a->type) {
	case ACT_MELEE:
		sprintf(buf, "%u-%u", a->data.melee.d_count, a->data.melee.d_type * a->data.melee.d_count);
		send_stuff(*((int *)p), " dealing ", cb,  buf, cbo, " of physical damage\n");
		break;
	case ACT_SPELL:
		if (a->data.spell.specialization == SP_NONE) {
			sprintf(buf, "%u-%u", a->data.spell.d_count, a->data.spell.d_type * a->data.spell.d_count);
			send_stuff(*((int *)p), " healing ", cb, buf, cbo, " HP\n");
		} else {
			sprintf(buf, "%u-%u", a->data.spell.d_count, a->data.spell.d_type * a->data.spell.d_count);
			send_stuff(*((int *)p), " dealing ", cb, buf, cbo, " of ", ser_spec(a->data.spell.specialization), " damage\n");
		}
	case ACT_BUFF:
		{
		struct attrs at = a->data.buff.d_attrs;
		sprintf(buf, "%d STR, %d DEF, %d CON, %d MAG, %d DEX, %d LCK",
			at.STR, at.DEF, at.CON, at.MAG, at.DEX, at.LCK);
		send_stuff(*((int *)p), " changing the attributes as follows: ", buf, "\n");
		}
	}
}


void *conn_handler(void *parm) {
	int tsd, len;
	tsd = *((int *)parm);

	char ip[INET_ADDRSTRLEN];
	struct sockaddr_in peeraddr;
	socklen_t peeraddrlen = sizeof(peeraddr);
	getpeername(tsd, (struct sockaddr *)&peeraddr, &peeraddrlen);
	inet_ntop(AF_INET, &(peeraddr.sin_addr), ip, INET_ADDRSTRLEN);

	char buf[MAXRCVLEN + 1];
	char *name;
	char *err_cl = format_str(C_RED, C_FG), *succ_cl = format_str(C_GREEN, C_FG);
	char *req_cl = format_str(C_MAGENTA, C_FG), *def_cl = format_str(C_DEFAULT, C_FG);
	int argc, player_number, n;
	char **argv, *cls;
	if (server_full()) {
		send_stuff(tsd, err_cl, "Sorry, the server is already full\n", def_cl);
		goto thr_ccls;
	}

	memset(buf, 0, MAXRCVLEN + 1);
	do
		send_stuff(tsd, req_cl, "Welcome, Player! Please provide your name > ", def_cl);
	while ((len = recv(tsd, buf, MAXRCVLEN, 0) < 3));
	argc = split_args(buf, &argv, ' ');
	name = strdup(argv[0]);
	free_args(argc, argv);

	player_number = add_player(name, tsd);
	if (!player_number) {
		send_stuff(tsd, err_cl, "Could not add you! Go home!\n", def_cl);
		goto thr_ccls;
	}
	n = player_number - 1;
	sprintf(buf, "%d", player_number);
	send_stuff(tsd, succ_cl, "Hi, ", name, " you are player #", buf, ".\n", def_cl);

	if (!players[n].character)
		send_stuff(tsd, req_cl, "You have no character yet. Please provide the class name",
			" (or the '.list' word).\n", def_cl);
	while (!players[n].character) {
		send_stuff(tsd, req_cl, "> ", def_cl);
		memset(buf, 0, MAXRCVLEN + 1);
		len = recv(tsd, buf, MAXRCVLEN, 0);
		if (!len)
			goto thr_cls;
		if (len < 3)
			continue;
		argc = split_args(buf, &argv, ' ');
		cls = strdup(argv[0]);
		if (!strcasecmp(".list", cls)) {
			send_stuff(tsd, succ_cl, "The following classes are defined: ");
			list_foreach(get_classes(), send_class, &tsd);
			send_stuff(tsd, "\n");
		} else {
			struct pk_class *cl = get_class(cls);
			if (!cl) {
				send_stuff(tsd, err_cl, "No such class\n", def_cl);
				continue;
			} else {
				send_stuff(tsd, succ_cl, "You have chosen the class ", cls, def_cl, "\n");
				players[n].character = gen_pokemon(cl, global_level);
			}
		}
	}

	send_stuff(tsd, succ_cl, ser_pokemon(players[n].character), "\n", def_cl);
	memset(buf, 0, MAXRCVLEN + 1);
	send_stuff(tsd, req_cl, "> ", def_cl);
	while ((len = recv(tsd, buf, MAXRCVLEN, 0))) {
		if (len < 3)
			goto contt;
		argc = split_args(buf, &argv, ' ');
		if (!strcasecmp(argv[0], ".say")) {
			char *c = join_args(argc - 1, argv + 1, ' ');
			chat(n, c);
			free(c);
		} else if (!strcasecmp(argv[0], ".help")) {
			send_stuff(tsd, succ_cl, "Available commands are: .help, .say, .quit, .status, .info, .skills, and action names\n", def_cl);
		} else if (!strcasecmp(argv[0], ".quit")) {
			broadcast(players[n].name, " has quit!\n");
			goto thr_cls;
		} else if (!strcasecmp(argv[0], ".skills")) {
			send_stuff(tsd, succ_cl);
			list_foreach(players[n].character->skills, send_skill, &tsd);
			send_stuff(tsd, def_cl);
		} else if (!strcasecmp(argv[0], ".status")) {
			send_status(n);
		} else if (*argv[0] == '.') {
			send_stuff(tsd, err_cl, "Unknown command!\n", def_cl);
		} else {
			char *sk = join_args(argc, argv, ' ');
			struct action *a = list_search(sk,
				players[n].character->skills, act_cmp);
			free(sk);
			if (!a) {
				send_stuff(tsd, err_cl, "Unknown skill!\n", def_cl);
				goto cont;
			}
			serv_attack(n, a);
		}
	cont:
		free_args(argc, argv);
	contt:
		memset(buf, 0, MAXRCVLEN + 1);
		send_stuff(tsd, req_cl, "> ", def_cl);
	}
	if (kaput)
		pthread_exit(0);

thr_cls:
	players[n].is_online = false;
	players[n].tsd = 0;
thr_ccls:
	close(tsd);
	pthread_exit(0);
}
