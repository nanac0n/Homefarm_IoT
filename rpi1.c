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
#include <sys/stat.h>
#include <sys/types.h>

// I2C 주소 정의
#define I2C_ADDR 0x27
#define LCD_CHR 1 // Mode - Sending data
#define LCD_CMD 0 // Mode - Sending command

// LCD 정의
#define LCD_LINE_1 0x80 // 1st line
#define LCD_LINE_2 0xC0 // 2nd line

// 비트 정의
#define LCD_BACKLIGHT 0x08  // On
#define ENABLE 0b00000100 // Enable bit

// 버튼 GPIO PIN 번호
#define PIN 20
#define POUT 21

// 식물 재배 GPIO PIN 번호
#define PINK_LED_PIN 26

// 물탱크 부족 GPIO PIN 번호
#define BLUE_LED_PIN 6

// GPIO 설정값
#define IN 0
#define OUT 1
#define LOW 0
#define HIGH 1
#define VALUE_MAX 40
#define DIRECTION_MAX 40

// 소켓 통신 설정
#define SERVER_IP "192.168.91.9"
#define PORT 2586
#define MAXLINE 1024

// 소켓 파일 디스크립터
int sockfd;
struct sockaddr_in servaddr;

// 전역 변수 정의
int lcd_fd = 0;
int FillWaterPump = 0;
int PlantFullyGrown = 0;
int temp = 0;
int humid = 0;
int LEDStatus = 0;
char PlantName[MAXLINE];
char PlantDate[MAXLINE];

// 데이터 구조체 정의
typedef struct {
    int temp;
    int humid;
    int LEDStatus;
} PlantData;

// GPIO 핀을 시스템에 내보내기
static int GPIOExport(int pin) {
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

// GPIO 핀을 시스템에서 제거
static int GPIOUnexport(int pin) {
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

// GPIO 핀 방향 설정 (0 = 입력, 1 = 출력)
static int GPIODirection(int pin, int dir) {
    static const char s_directions_str[] = "in\0out"; // 방향 문자열
    char path[DIRECTION_MAX]; // 경로 버퍼
    int fd; // 파일 디스크립터

    // GPIO 방향 파일 경로 설정
    snprintf(path, DIRECTION_MAX, "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY); // 방향 파일 열기
    if (fd == -1) {
        // 파일 열기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to open gpio direction for writing!\n");
        return -1;
    }

    // 방향 설정 (입력 또는 출력)
    if (write(fd, &s_directions_str[IN == dir ? 0 : 3], IN == dir ? 2 : 3) == -1) {
        fprintf(stderr, "Failed to set direction!\n");
        return -1;
    }

    close(fd); // 파일 디스크립터 닫기
    return 0; // 성공적으로 방향 설정 완료
}

// GPIO 핀 값 읽기
static int GPIORead(int pin) {
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

// GPIO 핀 값 쓰기
static int GPIOWrite(int pin, int value) {
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

void lcd_string(const char *message) {
    while (*message) { // 문자열의 끝까지 반복
        lcd_byte(*(message++), LCD_CHR); // 각 문자를 LCD에 전송
    }
}

void lcd_clear() {
    lcd_byte(0x01, LCD_CMD); // 화면 지우기 명령 전송
    usleep(500); // 명령 처리 대기
}

/***************************************************************************
 * dispose_button(void *arg)
 * 쓰레드 cancel시 호출될 함수
 * 연결된 GPIO핀을 제거함
 ***************************************************************************/
void dispose_button(void *arg) {
    // 할당한 자원 해제
    pthread_t *thread_id = (pthread_t*)arg; // 스레드 ID 포인터 가져오기
    
    // GPIO 핀 unexport
    GPIOUnexport(PIN);
    GPIOUnexport(POUT);

    free(thread_id); // 스레드 ID 메모리 해제
}

/***************************************************************************
 * button_control_thread(void* arg)
 * 버튼 스레드 함수
 * 버튼이 클릭되면 LCD에 식물이름, 심은 날짜, 온도, 습도, LED 상태 보여줌
 ***************************************************************************/
void* button_control_thread(void* arg) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL); // 쓰레드 취소 가능 상태 설정
    pthread_cleanup_push(dispose_button, arg);

    int prev_state = GPIORead(PIN); // 이전 버튼 상태, 1은 버튼이 눌리지 않은 상태로 초기화
    char buf[16];

    while (1) {
        int state = GPIORead(PIN); // 버튼 상태 읽기

        // 상태 변경인 경우
        if(state == LOW && prev_state == HIGH){ // 버튼이 눌러졌을 때(상태가 HIGH에서 LOW로 변경)
            printf("button ON\n");
            char response[MAXLINE];

            // 서버로부터 최신 정보 요청
            send(sockfd, "PLANT UPDATE", strlen("PLANT UPDATE"), 0);

            sleep(1);

            printf("Updated Plant Info\n");
            printf("%d %d %d\n", temp, humid, LEDStatus);

            // LCD 초기화
            lcd_init();

            // 첫 번째 정보 표시
            lcd_byte(LCD_LINE_1, LCD_CMD);
            lcd_string(PlantName);
            lcd_byte(LCD_LINE_2, LCD_CMD);
            lcd_string(PlantDate);
            sleep(2); // 2초 동안 표시
            lcd_clear();

            // 두 번째 정보 표시
            snprintf(buf, sizeof(buf), "T:%.1fC H:%.1f%%", temp/10.0, humid/10.0);
            lcd_byte(LCD_LINE_1, LCD_CMD);
            lcd_string(buf);

            if (LEDStatus == 0) {
                snprintf(buf, sizeof(buf), "LED OFF");
            } else if (LEDStatus == 1) {
                snprintf(buf, sizeof(buf), "LED ON");
            }
            lcd_byte(LCD_LINE_2, LCD_CMD);
            lcd_string(buf);

            sleep(2); // 2초 동안 표시

            // LCD 클리어
            lcd_clear();
        }
        prev_state = state; // 이전 상태 업데이트
        usleep(50000); // 0.1초 대기
    }
    pthread_cleanup_pop(1); // 쓰레드 종료 시 정리 함수 호출
    return NULL; // 스레드 종료
}

/***************************************************************************
 * init_button_control()
 * 버튼 초기화 함수
 * 버튼 스레드를 생성함
 ***************************************************************************/
pthread_t* init_button_control() {
    printf("Button thread start!\n"); // 초기화 메시지 출력
    pthread_t *thread_id = (pthread_t*)malloc(sizeof(pthread_t)); // 스레드 ID 메모리 할당
    if (thread_id == NULL) {
        fprintf(stderr, "Failed to allocate memory for thread ID\n");
        return NULL;
    }

    // GPIO 핀 내보내기
    if (GPIOExport(POUT) == -1 || GPIOExport(PIN)) {
        free(thread_id);
        return NULL;
    }

    usleep(1000 * 200); // 설정 후 잠시 대기

    // GPIO 핀 방향 설정
    if (GPIODirection(POUT, OUT) == -1 || GPIODirection(PIN, IN) == -1) {
        free(thread_id);
        return NULL;
    }

    // GPIO 핀 초기 상태로 설정 (초기값을 HIGH로 설정)
    if (GPIOWrite(POUT, 1) == -1) {
        free(thread_id);
        return NULL;
    }

    // 버튼, LCD 관리 스레드 생성
    if (pthread_create(thread_id, NULL, button_control_thread, (void*)thread_id) != 0) {
        free(thread_id); // 실패 시 메모리 해제
        return NULL;
    }

    return thread_id; // 성공적으로 초기화 완료
}

/***************************************************************************
 * request_and_receive(const char* request, char* response)
 * 서버에 요청하고자 하는 정보 요청하고 응답 받는 함수
 * 응답 받는 명령어를 포인터로 반환함
 ***************************************************************************/
void request_and_receive(const char* request, char* response) {
    char buffer[MAXLINE]; // 요청을 저장할 버퍼
    int n;

    // 서버로 요청 전송
    snprintf(buffer, sizeof(buffer), "%s", request); // 요청을 버퍼에 저장
    send(sockfd, buffer, strlen(buffer), 0); // 서버로 요청 전송

    // 서버로부터 응답 수신
    n = recv(sockfd, response, MAXLINE, 0); // 서버로부터 응답 받기
    if (n < 0) { // 응답 수신 실패 시
        perror("Receive failed"); // 오류 처리
        exit(1); // 프로그램 종료
    }
    response[n] = '\0'; // 응답 문자열 종료 처리
}

/***************************************************************************
 * socket_communication()
 * 서버와 소켓 통신을 통해 명령을 수신하고 처리하는 함수
 * 명령 수신에 따라 각각의 기능 작동
 ***************************************************************************/
void socket_communication() {
    char buffer[MAXLINE]; // 수신 버퍼
    int n; // 수신 바이트 수
    pthread_t *water_led_thread = NULL; // 스레드 포인터 선언

    // 소켓 연결되는 순간 -> 식물 이름, 날짜받기
    char response[MAXLINE];
    // 식물 이름 받기
    request_and_receive("PlantName", response);
    strncpy(PlantName, response, MAXLINE);
    // 식물 심은 날짜 받기
    request_and_receive("PlantDate", response);
    strncpy(PlantDate, response, MAXLINE);

    // 이전에 사용하던 GPIO로부터 발생하는 에러 해결하기 위한 코드
    // /***********************************/
    // GPIOExport(BLUE_LED_PIN);
    // usleep(1000 * 200);
    // GPIODirection(BLUE_LED_PIN, OUT);
    // GPIOWrite(BLUE_LED_PIN, LOW);
    // GPIOUnexport(BLUE_LED_PIN);

    // GPIOExport(PINK_LED_PIN);
    // usleep(1000 * 200);
    // GPIODirection(PINK_LED_PIN, OUT);
    // GPIOWrite(PINK_LED_PIN, LOW);
    // GPIOUnexport(PINK_LED_PIN);
    // /***********************************/

    // 끝날 때까지 반복
    while (1) {
        n = recv(sockfd, buffer, MAXLINE - 1, 0); // 소켓으로부터 데이터 수신
        if (n < 0) {
            perror("recv failed");
            break;
        }
        buffer[n] = '\0'; // 문자열 종료

        // 물 부족인 상태가 들어오는 경우
        if(strcmp(buffer, "WATER LOW") == 0) {
            // GPIO 핀 내보내기
            GPIOExport(BLUE_LED_PIN);

            usleep(1000 * 200); // 설정 후 잠시 대기

            // GPIO 핀 방향 설정
            GPIODirection(BLUE_LED_PIN, OUT);

            printf("Water low led\n");
            GPIOWrite(BLUE_LED_PIN, HIGH); // LED 켜기
        } else if (strcmp(buffer, "WATER OK") == 0) {
            // 물 정상인 상태가 들어오는 경우
            printf("Water ok\n");
            // LED 끄기
            if (GPIOWrite(BLUE_LED_PIN, LOW) == -1) {
                // 에러 처리
                continue;
            } else {
                // GPIO 핀 unexport
                GPIOUnexport(BLUE_LED_PIN);
            }
        } else if (strcmp(buffer, "Grow OK") == 0) {
            // 식물 재배 가능이 들어오는 경우
            // GPIO 핀 내보내기
            GPIOExport(PINK_LED_PIN);

            usleep(1000 * 200); // 설정 후 잠시 대기

            // GPIO 핀 방향 설정
            GPIODirection(PINK_LED_PIN, OUT);

            printf("grow led on\n");
            GPIOWrite(PINK_LED_PIN, HIGH); // LED 켜기
        } else if (strcmp(buffer, "PLANT DATA") == 0) {
            // 버튼 클릭으로 식물 정보가 들어오는 경우
            printf("Plant update to by server\n");
            PlantData plantData;
            n = recv(sockfd, &plantData, sizeof(PlantData), 0);
            if (n < 0) {
                perror("recv failed");
                break;
            }
            temp = plantData.temp;
            humid = plantData.humid;
            LEDStatus = plantData.LEDStatus;
        }
    }
}

/***************************************************************************
 * clean_and_clear()
 * 프로그램 종료 시 GPIO 정리하는 함수
 ***************************************************************************/
void clean_and_clear() {
    GPIOWrite(BLUE_LED_PIN, LOW);
    GPIOWrite(PINK_LED_PIN, LOW);
    GPIOWrite(PIN, OUT);
    GPIOUnexport(BLUE_LED_PIN);
    GPIOUnexport(PINK_LED_PIN);
    GPIOUnexport(PIN);
    GPIOUnexport(POUT);
}

/***************************************************************************
 * main(int argc, char *argv[]) 
 * 서버와 소켓 통신 연결
 ***************************************************************************/
int main(int argc, char* argv[]) {
    struct sockaddr_in serv_addr;
    char buffer[1024] = {0};
    int fd = open("/dev/i2c-1", O_RDWR);

    // 소켓 파일 디스크립터 생성
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // 서버 주소 변환
    if (inet_pton(AF_INET, "192.168.91.9", &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported \n");
        return -1;
    }

    // 서버에 연결 요청
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed \n");
        return -1;
    }
    printf("Socket Connection Complete!\n");

    // 버튼 스레드가 NULL이면 버튼 스레드 생성해줌
    pthread_t *button_thread = NULL;
    if (!button_thread) {
        button_thread = init_button_control();
        if (button_thread == NULL) {
            printf("Failed to initialize button control\n"); // 실패 메시지 출력
        }
    }

    // 소켓 통신을 통해 명령 수신 및 처리
    socket_communication();

    // 스레드가 종료될 때까지 대기
    if (button_thread) {
        pthread_join(*button_thread, NULL);
        free(button_thread);
    }

    clean_and_clear();

    close(sockfd);

    return 0;
}