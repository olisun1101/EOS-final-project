#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdbool.h>

#define PORT 8888
#define MAX_CLIENTS 5
#define NUM_WIRES 4
#define BUFFER_SIZE 1024
#define TOTAL_HINTS 5

// ==========================================
// 1. 共享資源與狀態定義 (Shared Game State)
// ==========================================
typedef struct {
	int countdown;
	int correct_wire; // 正確的線
	int boom_wire; // 會爆炸的線
	bool wire_locked[NUM_WIRES];
	int wire_lock_owner
		[NUM_WIRES]; // 記錄是哪個 Client ID 鎖定的 (-1 代表未鎖定)
	bool wire_cut[NUM_WIRES];
	int player_hint_quota[MAX_CLIENTS];
	bool hint_used[TOTAL_HINTS];
	bool game_over;
	bool success;
} GameState;

GameState game;

// ==========================================
// 2. 同步機制 (Synchronization Primitives)
// ==========================================
// 保護整個 GameState 關鍵區域的互斥鎖
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

// 模擬 Semaphore：限制全場同時只能有 2 把「虛擬剪線鉗」被佔用
pthread_mutex_t cutter_mutex = PTHREAD_MUTEX_INITIALIZER;
// 預定義提示內容
char hint_pool[TOTAL_HINTS][128];
int available_cutters = 2;

// 管理連線 Client 的鎖
int client_sockets[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// ==========================================
// 3. 函式宣告
// ==========================================
void *timer_thread_func(void *arg);
void *client_handler(void *arg);
void broadcast_message(const char *msg);
void send_status(int socket_fd);

// ==========================================
// 4. 主程式 (Main Server Loop)
// ==========================================
int main()
{
	int server_fd, new_socket;
	struct sockaddr_in address;
	int opt = 1;
	int addrlen = sizeof(address);
	srand(time(NULL));
	game.correct_wire = rand() % NUM_WIRES;
	do {
		game.boom_wire = rand() % NUM_WIRES;
	} while (game.boom_wire == game.correct_wire); // 確保不會重複
	// 初始化遊戲狀態
	int safe_wires[2];
	int safe_idx = 0;
	for (int i = 0; i < NUM_WIRES; i++) {
		if (i != game.correct_wire && i != game.boom_wire) {
			safe_wires[safe_idx++] = i;
		}
	}

	// 根據實際答案動態填入提示
	snprintf(hint_pool[0], 128, "💡 [提示] 熱感應顯示，正確線路是%s。",
		 (game.correct_wire % 2 == 0) ? "偶數" : "奇數");
	snprintf(hint_pool[1], 128, "💡 [提示] 電壓顯示，引爆線路是%s。",
		 (game.boom_wire % 2 == 0) ? "偶數" : "奇數");
	snprintf(hint_pool[2], 128,
		 "💡 [提示] 偵測到微弱訊號，正確線路絕對不是編號 %d。",
		 safe_wires[0]);
	snprintf(hint_pool[3], 128,
		 "💡 [提示] 根據波形分析，爆炸線路不是編號 %d。",
		 safe_wires[1]);
	snprintf(hint_pool[4], 128,
		 "💡 [提示] 專家指出，正確線路與爆炸線路的號碼總和為 %d。",
		 game.correct_wire + game.boom_wire);

	game.countdown = 60; // 60 秒倒數
	for (int i = 0; i < NUM_WIRES; i++) {
		game.wire_locked[i] = false;
		game.wire_lock_owner[i] = -1;
		game.wire_cut[i] = false;
	}
	game.game_over = false;
	game.success = false;

	// 建立 Socket (IPv4, TCP)
	if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
		perror("Socket failed");
		exit(EXIT_FAILURE);
	}

	// 設定 Socket 選項，允許重複使用 Port 避免 Address already in use
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt,
		       sizeof(opt))) {
		perror("setsockopt failed");
		exit(EXIT_FAILURE);
	}

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY; // 監聽所有網路介面
	address.sin_port = htons(PORT);

	// 綁定 Port
	if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		perror("Bind failed");
		exit(EXIT_FAILURE);
	}

	// 開始監聽
	if (listen(server_fd, MAX_CLIENTS) < 0) {
		perror("Listen failed");
		exit(EXIT_FAILURE);
	}

	printf("【Server】拆彈系統伺服器已啟動，正在監聽 Port %d...\n", PORT);
    // 開發者後門：可以在 Server 終端機先偷看答案確認提示是否正確
    printf("【Debug】正確線: %d, 爆炸線: %d\n", game.correct_wire, game.boom_wire);
	// 核心 OS 概念：啟動獨立的 Timer Thread 負責倒數計時
	pthread_t timer_tid;
	if (pthread_create(&timer_tid, NULL, timer_thread_func, NULL) != 0) {
		perror("Failed to create timer thread");
		exit(EXIT_FAILURE);
	}
	pthread_detach(timer_tid); // 讓系統在執行緒結束時自動回收資源

	// 主執行緒：連線監聽迴圈 (Accept Loop)
	while (!game.game_over) {
		if ((new_socket = accept(server_fd, (struct sockaddr *)&address,
					 (socklen_t *)&addrlen)) < 0) {
			if (game.game_over)
				break;
			perror("Accept failed");
			continue;
		}

		pthread_mutex_lock(&clients_mutex);
		if (client_count < MAX_CLIENTS) {
			client_sockets[client_count] = new_socket;

			// 配置獨立記憶體給 Client ID，避免 Race Condition
			int *client_id = malloc(sizeof(int));
			*client_id = client_count;
			client_count++;

			printf("【Server】玩家 (ID: %d) 已連線進來。\n",
			       *client_id);

			// 核心 OS 概念：每個連線配置一個 Thread (Connection-per-thread 模型)
			pthread_t client_tid;
			pthread_create(&client_tid, NULL, client_handler,
				       (void *)client_id);
			pthread_detach(client_tid);
		} else {
			char *msg = "Server Full. Disconnecting.\n";
			send(new_socket, msg, strlen(msg), 0);
			close(new_socket);
		}
		pthread_mutex_unlock(&clients_mutex);
	}

	close(server_fd);
	printf("【Server】遊戲結束，伺服器關閉。\n");
	return 0;
}

// ==========================================
// 5. 獨立計時執行緒 (Timer Thread)
// ==========================================
void *timer_thread_func(void *arg)
{
	while (!game.game_over) {
		sleep(1); // 每秒觸發一次

		// 鎖定關鍵區域，更新倒數時間
		pthread_mutex_lock(&state_mutex);
		if (!game.game_over) {
			game.countdown--;

			// 呼叫隊友黃同學的實體 Driver API（此處用 printf 模擬）
			// update_7seg_display(game.countdown);

			if (game.countdown <= 0) {
				game.game_over = true;
				game.success = false;
				printf("【BOOM】時間到！炸彈爆炸！\n");
				broadcast_message(
					"GAME_OVER: BOOM! Time expired.\n");
			} else {
				// 每秒對所有玩家廣播目前的倒數時間
				char status_msg[128];
				snprintf(status_msg, sizeof(status_msg),
					 "TIME: %d\n", game.countdown);
				broadcast_message(status_msg);
			}
		}
		pthread_mutex_unlock(&state_mutex);
	}
	return NULL;
}

// ==========================================
// 6. 玩家指令處理執行緒 (Client Handler Thread)
// ==========================================
void *client_handler(void *arg)
{
	int id = *(int *)arg;
	free(arg); // 釋放主執行緒配置的記憶體
	int my_socket;

	pthread_mutex_lock(&clients_mutex);
	my_socket = client_sockets[id];
	game.player_hint_quota[id] = 1; // 每位玩家初始有 1 次提示機會
	pthread_mutex_unlock(&clients_mutex);

	char buffer[BUFFER_SIZE];
	send_status(my_socket);

	while (!game.game_over) {
		memset(buffer, 0, BUFFER_SIZE);
		int valread = read(my_socket, buffer, BUFFER_SIZE);

		if (valread <= 0) {
			// 玩家斷線處理
			printf("【Server】玩家 %d 斷線。\n", id);

			// 自動釋放該玩家之前鎖定的所有資源（預防死鎖 Deadlock）
			pthread_mutex_lock(&state_mutex);
			for (int i = 0; i < NUM_WIRES; i++) {
				if (game.wire_locked[i] &&
				    game.wire_lock_owner[i] == id) {
					game.wire_locked[i] = false;
					game.wire_lock_owner[i] = -1;
					printf("【Server】自動釋放斷線玩家 %d 的線路锁 %d\n",
					       id, i);
				}
			}
			pthread_mutex_unlock(&state_mutex);
			break;
		}

		
		if (strncmp(buffer, "GET_CUTTER", 10) == 0) {
			pthread_mutex_lock(&cutter_mutex);
			if (available_cutters > 0) {
				available_cutters--;
				send(my_socket, "CUTTER_ACK: Success\n", 20, 0);
				printf("【Semaphore】玩家 %d 取得剪線鉗。剩餘數量: %d\n",
				       id, available_cutters);
			} else {
				send(my_socket,
				     "CUTTER_REJECT: No cutters available\n",
				     36, 0);
			}
			pthread_mutex_unlock(&cutter_mutex);
		} else if (strncmp(buffer, "RELEASE_CUTTER", 14) == 0) {
			pthread_mutex_lock(&cutter_mutex);
			available_cutters++;
			send(my_socket, "CUTTER_RELEASED\n", 16, 0);
			printf("【Semaphore】玩家 %d 歸還剪線鉗。剩餘數量: %d\n",
			       id, available_cutters);
			pthread_mutex_unlock(&cutter_mutex);
		} else if (strncmp(buffer, "LOCK", 4) == 0) {
			int wire_id = atoi(&buffer[5]);
			if (wire_id < 0 || wire_id >= NUM_WIRES) {
				send(my_socket, "ERROR: Invalid Wire ID\n", 23,
				     0);
				continue;
			}

			// 核心演算法：競爭判定關鍵區域
			pthread_mutex_lock(&state_mutex);
			if (game.wire_cut[wire_id]) {
				send(my_socket, "REJECT: Wire already cut\n",
				     25, 0);
			} else if (game.wire_locked[wire_id]) {
				send(my_socket,
				     "REJECT: Wire already locked by others\n",
				     38, 0);
			} else {
				game.wire_locked[wire_id] = true;
				game.wire_lock_owner[wire_id] = id;
				send(my_socket, "LOCK_ACK: Success\n", 18, 0);
				printf("【Mutex】線路 %d 被玩家 %d 成功鎖定！\n",
				       wire_id, id);
			}
			pthread_mutex_unlock(&state_mutex);
		} else if (strncmp(buffer, "CUT", 3) == 0) {
			int wire_id = atoi(&buffer[4]);

			pthread_mutex_lock(&state_mutex);
			if (!game.wire_locked[wire_id] ||
			    game.wire_lock_owner[wire_id] != id) {
				// OS 概念：非法訪問。沒鎖定線路就不能剪！
				send(my_socket,
				     "REJECT: You must LOCK the wire first\n",
				     37, 0);
			} else {
				game.wire_cut[wire_id] = true;
				game.wire_locked[wire_id] = false; // 解鎖
				printf("【Logic】玩家 %d 剪斷了線路 %d！\n", id,
				       wire_id);

				// 假設第 2 條線是解體正確線路，第 0 條是炸彈引爆線路
				if (wire_id == game.correct_wire) {
					game.game_over = true;
					game.success = true;
					broadcast_message(
						"GAME_OVER: SUCCESS! Bomb defused!\n");
				} else if (wire_id == game.boom_wire) {
					game.game_over = true;
					game.success = false;
					broadcast_message(
						"GAME_OVER: BOOM! Wrong wire cut!\n");
				} else {
					game.player_hint_quota[id]++;
					send(my_socket,
					     "CUT_ACK: Safe wire cut, keep going, hint+1.\n",
					     36, 0);
				}
			}
			pthread_mutex_unlock(&state_mutex);
		} else if (strncmp(buffer, "HINT", 4) == 0) {
			send(my_socket,
			     "⌛ [系統] 正在解密提示資料，請稍候...\n", 45, 0);
			sleep(3); // waiting for hint decryption
			pthread_mutex_lock(&state_mutex);
			if (game.player_hint_quota[id] <= 0) {
				send(my_socket,
				     "❌ [系統] 提示配額不足！請先成功剪斷安全線路以獲得獎勵。\n",
				     60, 0);
			} else {
				// 尋找一個還沒用過的提示
				int hint_index = -1;
				int start_node = rand() % TOTAL_HINTS;
				for (int i = 0; i < TOTAL_HINTS; i++) {
					int idx =
						(start_node + i) % TOTAL_HINTS;
					if (!game.hint_used[idx]) {
						hint_index = idx;
						break;
					}
				}

				if (hint_index != -1) {
					game.hint_used[hint_index] = true;
					game.player_hint_quota[id]--;
					char out[256];
					snprintf(out, sizeof(out),
						 "%s (剩餘配額: %d)\n",
						 hint_pool[hint_index],
						 game.player_hint_quota[id]);
					send(my_socket, out, strlen(out), 0);
				} else {
					send(my_socket,
					     "⚠️ [系統] 已無更多線索可提供。\n",
					     40, 0);
				}
			}
			pthread_mutex_unlock(&state_mutex);
		}
	}

	close(my_socket);
	return NULL;
}

// ==========================================
// 7. 輔助工具：廣播狀態給全場
// ==========================================
void broadcast_message(const char *msg)
{
	pthread_mutex_lock(&clients_mutex);
	for (int i = 0; i < client_count; i++) {
		send(client_sockets[i], msg, strlen(msg), 0);
	}
	pthread_mutex_unlock(&clients_mutex);
}

void send_status(int socket_fd)
{
	char status[256];
	pthread_mutex_lock(&state_mutex);
	snprintf(status, sizeof(status), "Welcome! Bomb countdown: %d sec.\n",
		 game.countdown);
	pthread_mutex_unlock(&state_mutex);
	send(socket_fd, status, strlen(status), 0);
}