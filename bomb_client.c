#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8888
#define BUFFER_SIZE 1024

volatile int keep_running = 1;

// ==========================================
// 【核心修改】同步機制與共享提示字變數
// ==========================================
pthread_mutex_t prompt_mutex = PTHREAD_MUTEX_INITIALIZER; // 保護提示字的互斥鎖
char active_prompt[128] =
	"請輸入行動代號 (0-5) > "; // 目前畫面上應該顯示的提示字

void print_menu()
{
	printf("\n===================================\n");
	printf(" 🛠️  拆彈特工終端機介面 (C Version) 🛠️\n");
	printf("===================================\n");
	printf(" [1] 搶奪剪線鉗 (GET_CUTTER)\n");
	printf(" [2] 歸還剪線鉗 (RELEASE_CUTTER)\n");
	printf(" [3] 鎖定線路   (LOCK wire)\n");
	printf(" [4] 剪斷線路   (CUT wire)\n");
	printf(" [5] 獲取提示   (GET HINT)\n");
	printf(" [0] 退出系統\n");
	printf("===================================\n");
}

// ==========================================
// 背景接收訊息執行緒 (負責即時更新時間)
// ==========================================
void *receive_messages(void *socket_desc)
{
	int sock = *(int *)socket_desc;
	char buffer[BUFFER_SIZE];
	int read_size;

	while (keep_running &&
	       (read_size = recv(sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
		buffer[read_size] = '\0';

		// 鎖定關鍵區域：確保在印出伺服器訊息時，提示字不會錯亂
		pthread_mutex_lock(&prompt_mutex);

		// \r 回到行首，\033[K 清除當前行，然後印出伺服器廣播
		printf("\r\033[K\n🔔 來自伺服器: %s", buffer);

		// 【核心修改】動態印出當前「真正活躍」的提示字
		printf("%s", active_prompt);
		fflush(stdout);

		pthread_mutex_unlock(&prompt_mutex);
	}

	if (read_size == 0) {
		printf("\n💥 [系統] 伺服器已斷線或遊戲結束。\n");
	}
	keep_running = 0;
	exit(0);
	return NULL;
}

// ==========================================
// 主執行緒 (處理使用者選單輸入)
// ==========================================
int main()
{
	int client_socket;
	struct sockaddr_in server_addr;

	client_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (client_socket == -1) {
		perror("無法建立 Socket");
		return 1;
	}

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(SERVER_PORT);
	if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
		perror("無效的 IP 地址");
		return 1;
	}

	if (connect(client_socket, (struct sockaddr *)&server_addr,
		    sizeof(server_addr)) < 0) {
		perror("連線伺服器失敗");
		return 1;
	}
	printf("✅ 成功連線至拆彈伺服器！\n");

	pthread_t recv_thread;
	if (pthread_create(&recv_thread, NULL, receive_messages,
			   (void *)&client_socket) < 0) {
		perror("無法建立接收執行緒");
		return 1;
	}

	usleep(500000);

	char choice[10];
	char wire_id[10];
	char send_buffer[BUFFER_SIZE];

	while (keep_running) {
		print_menu();

		// 初始化主選單提示字
		pthread_mutex_lock(&prompt_mutex);
		strcpy(active_prompt, "請輸入行動代號 (0-5) > ");
		printf("%s", active_prompt);
		fflush(stdout);
		pthread_mutex_unlock(&prompt_mutex);

		if (fgets(choice, sizeof(choice), stdin) == NULL)
			break;
		choice[strcspn(choice, "\n")] = 0;

		if (strcmp(choice, "1") == 0) {
			send(client_socket, "GET_CUTTER\n", 11, 0);
		} else if (strcmp(choice, "2") == 0) {
			send(client_socket, "RELEASE_CUTTER\n", 15, 0);
		} else if (strcmp(choice, "5") == 0) {
			pthread_mutex_lock(&prompt_mutex);
			strcpy(active_prompt, "🔍 正在請求提示...請稍候...");
			printf("\r\033[K%s", active_prompt);
			fflush(stdout);
			pthread_mutex_unlock(&prompt_mutex);

			send(client_socket, "HINT\n", 5, 0);

			// 提示結束後會自動由背景 Thread 印出結果並恢復提示字
		}
		// ==========================================
		// 【核心修改】選擇 3: 鎖定線路
		// ==========================================
		else if (strcmp(choice, "3") == 0) {
			pthread_mutex_lock(&prompt_mutex);
			// 1. 將共享提示字改為鎖定線路專用（此處依據邏輯設定為鎖定提示，若欲完全遵照字面改為剪斷亦可修改此處字串）
			strcpy(active_prompt,
			       "🎯 請輸入要『鎖定』的線路編號 (0-3) > ");
			printf("\r\033[K%s", active_prompt);
			fflush(stdout);
			pthread_mutex_unlock(&prompt_mutex);

			if (fgets(wire_id, sizeof(wire_id), stdin) != NULL) {
				wire_id[strcspn(wire_id, "\n")] = 0;
				snprintf(send_buffer, sizeof(send_buffer),
					 "LOCK %s\n", wire_id);
				send(client_socket, send_buffer,
				     strlen(send_buffer), 0);
			}

			// 2. 輸入完成後，在離開 if 區塊前，將提示字改回原本的主選單提示
			pthread_mutex_lock(&prompt_mutex);
			strcpy(active_prompt, "請輸入行動代號 (0-5) > ");
			pthread_mutex_unlock(&prompt_mutex);
		}
		// ==========================================
		// 【核心修改】選擇 4: 剪斷線路
		// ==========================================
		else if (strcmp(choice, "4") == 0) {
			pthread_mutex_lock(&prompt_mutex);
			// 1. 將共享提示字改為剪斷線路專用
			strcpy(active_prompt,
			       "✂️ 請輸入要『剪斷』的線路編號 (0-3) > ");
			printf("\r\033[K%s", active_prompt);
			fflush(stdout);
			pthread_mutex_unlock(&prompt_mutex);

			if (fgets(wire_id, sizeof(wire_id), stdin) != NULL) {
				wire_id[strcspn(wire_id, "\n")] = 0;
				snprintf(send_buffer, sizeof(send_buffer),
					 "CUT %s\n", wire_id);
				send(client_socket, send_buffer,
				     strlen(send_buffer), 0);
			}

			// 2. 輸入完成後，恢復成原來的提示字
			pthread_mutex_lock(&prompt_mutex);
			strcpy(active_prompt, "請輸入行動代號 (0-5) > ");
			pthread_mutex_unlock(&prompt_mutex);
		} else if (strcmp(choice, "0") == 0) {
			printf("👋 登出系統...\n");
			keep_running = 0;
			break;
		} else {
			printf("⚠️ 無效的指令，請重新輸入。\n");
		}

		usleep(200000);
	}

	close(client_socket);
	return 0;
}