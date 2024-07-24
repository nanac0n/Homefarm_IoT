#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// 초음파센서, 온습도센서, 터치센서 핀번호 정의
#define TOUCH_PIN 9
#define ECHO_PIN 23
#define TRIG_PIN 24
#define DTH_PIN 27
#define VALUE_MAX 40
#define DIRECTION_MAX 40

// I2C 주소 정의
#define I2C_ADDR 0x27
#define LCD_CHR 1 
#define LCD_CMD 0 

// LCD 정의
#define LCD_LINE_1 0x80 
#define LCD_LINE_2 0xC0 

// 비트 정의
#define LCD_BACKLIGHT 0x08 //On
#define ENABLE 0b00000100 

// Socket 통신 관련 변수 정의
#define PORT 2586
#define MAXLINE 1024

// 전역 변수 정의
int distance = 0;
int temp = 0;
int humid = 0;
int IsPlantFullyGrown = 0;
int IsNeedMoreWater = 0;
int PrevWaterStatus = 0;
int LEDStatus = 0;
int lcd_fd = 0;

// 소켓 파일 디스크립터
int listenfd,connfd, connfd2;
int client1_connected = 0;
int client2_connected = 0;
int client2_sockfd;  
pthread_mutex_t connection_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t connection_cond = PTHREAD_COND_INITIALIZER;

struct sockaddr_in servaddr, cliaddr, cliaddr2;
socklen_t clilen, clilen2;

// 식물 data 구조체 정의
typedef struct {
    int temp;
    int humid;
    int LEDStatus;
} PlantData;
char PlantName[MAXLINE] = "Tomato";
char PlantDate[MAXLINE] = "2024-06-01";

// 에러 핸들링 함수
void error_handling(const char *message) {
    perror(message);
    exit(1);
}

// GPIO 핀을 시스템에 내보내기
void gpio_export(int pin) {
    char buffer[3]; // 핀 번호를 저장할 버퍼
    ssize_t bytes_written; // 쓰여진 바이트 수
    int fd; // 파일 디스크립터

    // GPIO export 파일 열기
    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd == -1) {
        // 파일 열기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to open export for writing!\n");
        return (-1);
    }

    // 핀 번호를 버퍼에 쓰기
    bytes_written = snprintf(buffer, 3, "%d", pin);
    write(fd, buffer, bytes_written); // 핀 번호를 export 파일에 쓰기
    close(fd); // 파일 디스크립터 닫기
    return 0; // 성공적으로 내보내기 완료
}

// GPIO 핀을 시스템에서 제거하기
void gpio_unexport(int pin) {
    char buffer[3]; // 핀 번호를 저장할 버퍼
    ssize_t bytes_written; // 쓰여진 바이트 수
    int fd; // 파일 디스크립터

    // GPIO unexport 파일 열기
    fd = open("/sys/class/gpio/unexport", O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "Failed to open unexport for writing!\n");
        return (-1);
    }

    // 핀 번호를 버퍼에 쓰기
    bytes_written = snprintf(buffer, 3, "%d", pin);
    write(fd, buffer, bytes_written); // 핀 번호를 unexport 파일에 쓰기
    close(fd); // 파일 디스크립터 닫기
    return 0; // 성공적으로 제거 완료
}

// GPIO 핀의 방향 설정
void gpio_direction(int pin, const char* dir) {
    char path[35];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("gpio/direction");
        return;
    }
    write(fd, dir, strlen(dir));
    close(fd);
}

// GPIO 핀의 값을 읽기
int gpio_read(int pin) {
    char path[VALUE_MAX]; // 경로 버퍼
    char value_str[3]; // 값 문자열 버퍼
    int fd; // 파일 디스크립터

    // GPIO 값 파일 경로 설정
    snprintf(path, VALUE_MAX, "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_RDONLY); // 값 파일 열기
    if (fd == -1) {
        // 파일 열기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to open gpio value for reading!\n");
        return -1;
    }

    // 값 읽기
    if (read(fd, value_str, 3) == -1) {
        // 값 읽기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to read value!\n");
        return -1;
    }

    close(fd); // 파일 디스크립터 닫기
    return atoi(value_str); // 값을 정수로 변환하여 반환
}

// GPIO 핀의 값을 쓰기
void gpio_write(int pin, int value) {
    static const char s_values_str[] = "01"; // 값 문자열
    char path[VALUE_MAX]; // 경로 버퍼
    int fd; // 파일 디스크립터

    // GPIO 값 파일 경로 설정
    snprintf(path, VALUE_MAX, "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_WRONLY); // 값 파일 열기
    if (fd == -1) {
        // 파일 열기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to open gpio value for writing!\n");
        return -1;
    }

    // 값 쓰기
    if (write(fd, &s_values_str[LOW == value ? 0 : 1], 1) != 1) {
        // 값 쓰기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to write value!\n");
        return -1;
    }

    close(fd); // 파일 디스크립터 닫기
    return 0; // 성공적으로 값 쓰기 완료
}

// LCD의 ENABLE 비트를 토글하는 함수
void lcd_toggle_enable(int bits) {
    usleep(500); // LCD가 명령을 처리할 수 있도록 짧은 지연 시간 추가
    if (write(lcd_fd, &bits, 1) != 1) { // bits를 LCD에 쓰기
        perror("lcd_toggle_enable - write 1"); // 쓰기 실패 시 오류 처리

    }
    bits |= ENABLE; // ENABLE 비트 설정
    if (write(lcd_fd, &bits, 1) != 1) { // ENABLE 비트를 설정한 후 bits를 다시 쓰기
        perror("lcd_toggle_enable - write 2"); // 쓰기 실패 시 오류 처리
    }
    usleep(500); // LCD가 명령을 처리할 수 있도록 짧은 지연 시간 추가
    bits &= ~ENABLE; // ENABLE 비트 해제
    if (write(lcd_fd, &bits, 1) != 1) { // ENABLE 비트를 해제한 후 bits를 다시 쓰기
        perror("lcd_toggle_enable - write 3"); // 쓰기 실패 시 오류 처리
    }
    usleep(500); // LCD가 명령을 처리할 수 있도록 짧은 지연 시간 추가
}

// LCD에 명령 또는 데이터를 보내는 함수
void lcd_byte(int bits, int mode) {
    int bits_high = mode | (bits & 0xF0) | LCD_BACKLIGHT; // 상위 4비트를 설정
    int bits_low = mode | ((bits << 4) & 0xF0) | LCD_BACKLIGHT; // 하위 4비트를 설정

    if (write(lcd_fd, &bits_high, 1) != 1) { // 상위 4비트를 LCD에 쓰기
        perror("lcd_byte - write 1"); // 쓰기 실패 시 오류 처리
    }
    lcd_toggle_enable(bits_high); // ENABLE 신호 토글

    if (write(lcd_fd, &bits_low, 1) != 1) { // 하위 4비트를 LCD에 쓰기
        perror("lcd_byte - write 2"); // 쓰기 실패 시 오류 처리
    }
    lcd_toggle_enable(bits_low); // ENABLE 신호 토글
}

// LCD 초기화 함수
void lcd_init() { // 초기화 시작 메시지 출력
    printf("lcd init\n");
    if ((lcd_fd = open("/dev/i2c-1", O_RDWR)) < 0) { // I2C 버스를 열기
        perror("Failed to open i2c bus"); // 열기 실패 시 오류 처리
        exit(1); // 프로그램 종료
    }
    if (ioctl(lcd_fd, I2C_SLAVE, I2C_ADDR) < 0) { // I2C 장치 설정
        perror("Failed to acquire bus access and/or talk to slave"); // 설정 실패 시 오류 처리
        exit(1); // 프로그램 종료
    }

    // LCD 초기화 명령 전송
    lcd_byte(0x33, LCD_CMD); // 초기 명령
    lcd_byte(0x32, LCD_CMD); // 4비트 모드 설정
    lcd_byte(0x06, LCD_CMD); // 커서 이동 방향 설정
    lcd_byte(0x0C, LCD_CMD); // 디스플레이 켜기, 커서 끄기
    lcd_byte(0x28, LCD_CMD); // 4비트 모드, 2라인, 5x7 포맷
    lcd_byte(0x01, LCD_CMD); // 화면 지우기
    usleep(500); // 명령 처리 대기
}

// LCD에 문자열을 쓰는 함수
void lcd_string(const char *message) {
    while (*message) { // 문자열의 끝까지 반복
        lcd_byte(*(message++), LCD_CHR); // 각 문자를 LCD에 전송
    }
}

// LCD 화면을 지우는 함수
void lcd_clear() {
    lcd_byte(0x01, LCD_CMD); // 화면 지우기 명령 전송
    usleep(500); // 명령 처리 대기
}

/***************************************************************************
 * delayMicroseconds()
 * 지정된 microsecond초 동안 지연하는 함수
 * 초음파 센서에 사용
 ***************************************************************************/
void delayMicroseconds(long delay) {
    struct timespec ts;
    ts.tv_sec = delay / 1000000;
    ts.tv_nsec = (delay % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

/***************************************************************************
 * micros()
 * 시간을 microsecond 단위로 변환하는 함수
 * 초음파 센서에 사용
 ***************************************************************************/
long micros() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/***************************************************************************
 * getDistance()
 * 초음파 센서를 사용하여 식물의 성장을 측정하는 함수
 ***************************************************************************/
int getDistance() {
    long startTime, endTime;
    float distance;

    // TRIG 핀에 펄스 발생
    gpio_write(TRIG_PIN, 1);
    delayMicroseconds(10);
    gpio_write(TRIG_PIN, 0);

    // ECHO 핀의 신호 대기
    while (gpio_read(ECHO_PIN) == 0);

    // 신호 수신 시간 기록
    startTime = micros();

    // 신호 수신 종료 시간 기록
    while (gpio_read(ECHO_PIN) == 1);
    endTime = micros();

    // 시간 차이 계산
    float travelTime = (float)(endTime - startTime) / 1000000;

    // 거리 계산 (음속: 34300 cm/s)
    distance = travelTime * 34300 / 2;

    return (int)distance;
}

/***************************************************************************
 * simulate_day(void* arg)
 * 타이머 동작 스레드 함수
 * 6시, 12시, 24시에 일조량 관리, 물공급 관리 알림 보냄
 * 매 시간마다 식물의 성장을 getDistance() 함수로 확인
 ***************************************************************************/
void* simulate_day(void* arg) {
    int sockfd = *(int*)arg;
    int hour = 0;
    int PlantGrownStatus = 0;
    char buffer[MAXLINE];

    while (1) { // 하루를 240초로 가정
        sleep(10);
        hour++;
        distance = getDistance();
        printf("현재 시간은 %d시 입니다.  \n ", hour);

        // 식물이 다 자란 경우
        // 다 자란 최초의 한번만 이벤트 발생 
        if (distance <  15 && PlantGrownStatus == 0) {
            IsPlantFullyGrown = 1;
            snprintf(buffer, MAXLINE, "Grow OK");
            send(client2_sockfd, buffer, strlen(buffer), 0);
            PlantGrownStatus = 1;
            // 이벤트 발생 시 client2에 데이터 전송
        }

        if (hour == 6) {
            printf("오전 %d시 - 일조량 관리 시작\n", hour);
            snprintf(buffer, MAXLINE, "LIGHT_START");
            send(sockfd, buffer, strlen(buffer), 0);
        }
        if (hour == 12) {
            printf("낮 %d시 - 물 공급 시작\n", hour);
            snprintf(buffer, MAXLINE, "WATER");
            send(sockfd, buffer, strlen(buffer), 0);
        }
        if (hour == 24) {
            printf("밤 %d시 - 일조량 관리 종료\n", hour);
            snprintf(buffer, MAXLINE, "LIGHT_END");
            send(sockfd, buffer, strlen(buffer), 0);
            LEDStatus = 0;
            hour = 0;
        }
    }
    return NULL;
}

/***************************************************************************
 * touch_monitor(void* arg)
 * 터치, LCD 스레드 함수
 * 터치센서를 사용하여, LCD 모니터를 제어함
 * 몇 번 클릭했는지에 따라, 화면을 다르게 표시함
 ***************************************************************************/
void* touch_monitor(void* arg) {
    int MonitorTHEME = 0;
    char buf[16];

    while (1) {
        if (gpio_read(TOUCH_PIN) == 1) {
            MonitorTHEME++;

            if (MonitorTHEME > 3) MonitorTHEME = 0;
            lcd_clear();
            switch (MonitorTHEME) {
                case 1:
                    printf("**Monitor THEME1 : WATER CONSUME**\n");
                    if (IsNeedMoreWater == 0){
                        lcd_byte(LCD_LINE_1, LCD_CMD);
                        lcd_string("Water Is Full"); //한줄에 16글자 가능
                        lcd_byte(LCD_LINE_2, LCD_CMD);
                        lcd_string("It's OK");
                    } else if (IsNeedMoreWater == 1){
                        lcd_byte(LCD_LINE_1, LCD_CMD);
                        lcd_string("Fill in the"); //한줄에 16글자 가능
                        lcd_byte(LCD_LINE_2, LCD_CMD);
                        lcd_string("WATER TANK");
                    }
                    break;
                case 2:
                    printf("**Monitor THEME2 : TODAY TEMP**\n");

                    snprintf(buf, sizeof(buf), "Temp: %.1fC", PlantData.temp / 10.0);
                    lcd_byte(LCD_LINE_1, LCD_CMD);
                    lcd_string(buf);

                    snprintf(buf, sizeof(buf), "Humid: %.1f%%", PlantData.humid / 10.0);
                    lcd_byte(LCD_LINE_2, LCD_CMD);
                    lcd_string(buf);

                    break;
                case 3:
                    printf("**Monitor THEME3 : LED STATE**\n");

                    lcd_byte(LCD_LINE_1, LCD_CMD);
                    lcd_string("LED STATE");
                    if (LEDStatus == 0) {
                        lcd_byte(LCD_LINE_2, LCD_CMD);
                        lcd_string("OFF");
                    } else if (LEDStatus == 1) {
                        lcd_byte(LCD_LINE_2, LCD_CMD);
                        lcd_string("ON");
                    }
                    break;

                default:
                    lcd_byte(LCD_LINE_1, LCD_CMD);
                    lcd_string("1 : WaterConsume");
                    lcd_byte(LCD_LINE_2, LCD_CMD);
                    lcd_string("2 : ENV  3 : LED");
                    break;
            }
            usleep(500000);
        }
        usleep(10000);
    }
}

/***************************************************************************
 * socket_communication()
 * 온습도센서에서 데이터를 읽어오는 함수
 * read_dht.py 파일에서 온습도를 측정하고, 데이터를 읽어와 temp, humid에 저장함
 ***************************************************************************/
void* read_dht(void* arg) {
    while (1) {
        FILE *fp = popen("python3 read_dht.py", "r");
        if (fp == NULL) {
            perror("Failed to RUN SCRIPT");
            exit(1);
        }

        char result[100];
        if (fgets(result, sizeof(result), fp) != NULL) {
            float temperature = 0.0, humidity = 0.0;
            int matched = sscanf(result, "%f,%f", &temperature, &humidity);
            if (matched == 2) {
                //printf("Temperature: %.1f C, Humidity: %.1f %%\n", temperature, humidity);
                PlantData.temp = (int)(temperature * 10);
                PlantData.humid = (int)(humidity * 10);

                printf("%d %d is WRITTEN BY PYTHON SCRIPT\n", PlantData.temp, PlantData.humid);
            } else {
                printf("Failed to sensor data\n");
            }
        } else {
            printf("Failed to read data from Python script\n");
        }

        pclose(fp);
        usleep(200000);
        }
}

/***************************************************************************
 * socket_communication_client1(void* arg)
 * rpi3과 소켓 통신을 통해 명령을 수신하고 처리하는 함수
 * 명령 수신에 따라 각각의 기능 작동
 ***************************************************************************/
void* socket_communication_client1(void* arg) {
    int sockfd = *(int*)arg;
    char buffer[MAXLINE];
    int n;

    while (1) {
        n = recv(sockfd, buffer, MAXLINE - 1, 0);
        if (n < 0) {
            perror("recv failed");
            break;
        }
        buffer[n] = '\0';

        if (strcmp(buffer, "LED ON") == 0) {
            LEDStatus = 1;
            printf("FROM RPI3 ::: LED ON \n");
        } else if (strcmp(buffer, "LED OFF") == 0) {
            LEDStatus = 0;
            printf("FROM RPI3 ::: LED OFF \n");

        } else if (strcmp(buffer, "WATER LOW") == 0) {
            IsNeedMoreWater = 1;
            if(PrevWaterStatus == 1 && IsNeedMoreWater == 1){
                snprintf(buffer, MAXLINE, "WATER LOW");
                send(client2_sockfd, buffer, strlen(buffer), 0);
                printf("FROM RPI3 ::: WATER LOW \n");
                PrevWaterStatus = 0;
            }

        } else if (strcmp(buffer, "WATER OK") == 0) {
            IsNeedMoreWater = 0;
            if(PrevWaterStatus == 0 && IsNeedMoreWater ==0){
                snprintf(buffer, MAXLINE, "WATER OK");
                send(client2_sockfd, buffer, strlen(buffer), 0);
                printf("FROM RPI3 ::: WATER OK \n");
                PrevWaterStatus = 1;
            }
        } 
        else if (strcmp(buffer, "TEMP") == 0) {
            snprintf(buffer, MAXLINE, "%d", PlantData.temp);
            send(sockfd, buffer, strlen(buffer), 0);
            printf("TO RPI3 ::: send TEMP \n");

        } else if (strcmp(buffer, "HUMID") == 0) {
            snprintf(buffer, MAXLINE, "%d", PlantData.humid);
            send(sockfd, buffer, strlen(buffer), 0);
            printf("TO RPI3 ::: send HUMID \n");
        }
        else {
            snprintf(buffer, MAXLINE, "UNKNOWN REQUEST");
            send(sockfd, buffer, strlen(buffer), 0);
        }
    }

    close(sockfd);
    return NULL;
}

/***************************************************************************
 * socket_communication_client2(void* arg)
 * rpi1과 소켓 통신을 통해 명령을 수신하고 처리하는 함수
 * 명령 수신에 따라 각각의 기능 작동
 ***************************************************************************/
void* socket_communication_client2(void* arg) {
    int sockfd = *(int*)arg;
    char buffer[MAXLINE];
    int n;

    while (1) {
        n = recv(sockfd, buffer, MAXLINE - 1, 0);
        if (n < 0) {
            perror("recv failed");
            break;
        }
        buffer[n] = '\0';
        if (strcmp(buffer, "PlantName") == 0) {
            snprintf(buffer, MAXLINE, PlantName);
            send(sockfd, buffer, strlen(buffer), 0);
            printf("TO RPI2 ::: send PlantName \n");

        } else if (strcmp(buffer, "PlantDate") == 0) {
            snprintf(buffer, MAXLINE, PlantDate);
            send(sockfd, buffer, strlen(buffer), 0);
            printf("TO RPI2 ::: send PlantDate \n");

        } else if (strcmp(buffer, "PLANT UPDATE") == 0) {
            printf("TO RPI2 ::: PLANT INFORM UPDATE\n");
            PlantData.LEDStatus = LEDStatus;
            send(sockfd, "PLANT DATA", strlen("PLANT DATA"), 0);
            send(sockfd, &PlantData, sizeof(PlantData), 0);
        } else {
            snprintf(buffer, MAXLINE, "UNKNOWN REQUEST");
            send(sockfd, buffer, strlen(buffer), 0);
        }
    }
    close(sockfd);
    return NULL;
}

/***************************************************************************
 * setup()
 * 사용할 GPIO 핀과 LCD를 초기화하는 함수
 ***************************************************************************/
void setup() {
    gpio_export(TRIG_PIN);
    gpio_export(ECHO_PIN);
    gpio_export(TOUCH_PIN);
    gpio_direction(TRIG_PIN, "out");
    gpio_direction(ECHO_PIN, "in");
    gpio_direction(TOUCH_PIN, "in");
    gpio_write(TRIG_PIN, 0);
    usleep(500000); // 0.5초 대기
    lcd_init();
}
/***************************************************************************
 * main()
 * rpi1, rpi3과 소켓 통신 연결
 * rpi1과 rpi3이 모두 연결되어야 다음으로 넘어감
 ***************************************************************************/
int main() {
    setup();
    pthread_t touch_change_monitor_thread, dht_thread, simulate_day_thread, client_thread1, client_thread2;
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        error_handling("socket creation failed");
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(PORT);

    if (bind(listenfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        error_handling("bind failed");
    }

    if (listen(listenfd, 5) < 0) {
        error_handling("listen failed");
    }
    printf("Server listening on port %d\n", PORT);

    // 첫 번째 클라이언트 연결 (192.168.91.20)
    clilen = sizeof(cliaddr);
    connfd = accept(listenfd, (struct sockaddr*)&cliaddr, &clilen);
    if (connfd < 0) {
        error_handling("accept failed");
    }
    printf("Connection accepted from %s:%d\n", inet_ntoa(cliaddr.sin_addr), ntohs(cliaddr.sin_port));
    pthread_mutex_lock(&connection_mutex);
    client1_connected = 1;
    pthread_cond_signal(&connection_cond);
    pthread_mutex_unlock(&connection_mutex);
    pthread_create(&client_thread1, NULL, socket_communication_client1, &connfd);

    // 두 번째 클라이언트 연결 (192.168.91.13)
    clilen2 = sizeof(cliaddr2);
    connfd2 = accept(listenfd, (struct sockaddr*)&cliaddr2, &clilen2);
    if (connfd2 < 0) {
        error_handling("accept failed");
    }
    printf("Connection accepted from %s:%d\n", inet_ntoa(cliaddr2.sin_addr), ntohs(cliaddr2.sin_port));
    // client2의 소켓 파일 디스크립터 저장
    client2_sockfd = connfd2;  
    pthread_mutex_lock(&connection_mutex);
    client2_connected = 1;
    pthread_cond_signal(&connection_cond);
    pthread_mutex_unlock(&connection_mutex);
    pthread_create(&client_thread2, NULL, socket_communication_client2, &connfd2);

    // 두 클라이언트가 모두 연결될때까지 대기
    pthread_mutex_lock(&connection_mutex);
    while (client1_connected == 0 || client2_connected == 0) {
        pthread_cond_wait(&connection_cond, &connection_mutex);
    }
    pthread_mutex_unlock(&connection_mutex);

    pthread_create(&simulate_day_thread, NULL, simulate_day, &connfd);
    pthread_create(&touch_change_monitor_thread, NULL, touch_monitor, NULL);
    pthread_create(&dht_thread, NULL, read_dht, NULL);

    pthread_join(simulate_day_thread, NULL);
    pthread_join(touch_change_monitor_thread, NULL);
    pthread_join(dht_thread, NULL);
    pthread_join(client_thread1, NULL);
    pthread_join(client_thread2, NULL);

    gpio_unexport(TRIG_PIN);
    gpio_unexport(ECHO_PIN);
    gpio_unexport(TOUCH_PIN);
    gpio_unexport(DTH_PIN);

    close(lcd_fd);
    close(listenfd);

    return 0;
}