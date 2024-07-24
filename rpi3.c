#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>

// 서보모터 PWM 번호
#define SERVO_PWM 0

// 물공급 관리 GPIO PIN 번호
#define WATER_SUPPLY_PIN 12

// 물탱크 GPIO PIN 번호
#define WATER_LEVEL_PIN 25
#define LED_PIN 23
#define BUZZER_PIN 16

// 일조량 관리 GPIO PIN 번호
#define LIGHT_SENSOR_PIN 17
#define LED2_PIN 27

// GPIO 설정 값
#define IN 0
#define OUT 1
#define LOW 0
#define HIGH 1
#define VALUE_MAX 256
#define DIRECTION_MAX 256

// 소켓 통신 설정
#define PORT 2586
#define MAXLINE 1024

// 소켓 파일 디스크립터
int sockfd;
struct sockaddr_in servaddr, cliaddr;

// 온도, 습도 저장할 전역변수
float temp;
float humid;

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
        return -1;
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
        // 파일 열기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to open unexport for writing!\n");
        return -1;
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

// PWM 채널 시스템에 내보내기
static int PWMExport(int pwmnum) {
    char buffer[3]; // PWM 번호 버퍼
    int fd, byte; // 파일 디스크립터와 바이트 수

    // PWM export 파일 열기
    fd = open("/sys/class/pwm/pwmchip0/export", O_WRONLY);
    if (fd == -1) {
        // 파일 열기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to open export for export!\n");
        return -1;
    }

    // PWM 번호 버퍼에 쓰기
    byte = snprintf(buffer, 3, "%d", pwmnum);
    write(fd, buffer, byte); // PWM 번호를 export 파일에 쓰기
    close(fd); // 파일 디스크립터 닫기

    sleep(1); // 설정 후 잠시 대기

    return 0; // 성공적으로 내보내기 완료
}

// PWM 채널 시스템에서 제거
static int PWMUnexport(int pwmnum) {
    char buffer[3];
    ssize_t bytes_written;
    int fd;
    fd = open("/sys/class/pwm/pwmchip0/unexport", O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "Failed to open in unexport!\n");
        return -1;
    }
    bytes_written = snprintf(buffer, 3, "%d", pwmnum);
    write(fd, buffer, bytes_written);
    close(fd);
    sleep(1);
    return 0;
}

// PWM 활성화 함수
static int PWMEnable(int pwmnum) {
    static const char s_enable_str[] = "1"; // 활성화 문자열

    char path[DIRECTION_MAX]; // 경로 버퍼
    int fd; // 파일 디스크립터

    // PWM enable 파일 경로 설정
    snprintf(path, DIRECTION_MAX, "/sys/class/pwm/pwmchip0/pwm%d/enable", pwmnum);
    fd = open(path, O_WRONLY); // enable 파일 열기
    if (fd == -1) {
        // 파일 열기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to open in enable!\n");
        return -1;
    }

    write(fd, s_enable_str, strlen(s_enable_str)); // enable 파일에 쓰기
    close(fd); // 파일 디스크립터 닫기

    return 0; // 성공적으로 활성화 완료
}

// PWM 비활성화 함수
static int PWMDisable(int pwmnum) {
    static const char s_disable_str[] = "0"; // 비활성화 문자열

    char path[DIRECTION_MAX]; // 경로 버퍼
    int fd; // 파일 디스크립터

    // PWM disable 파일 경로 설정
    snprintf(path, DIRECTION_MAX, "/sys/class/pwm/pwmchip0/pwm%d/enable", pwmnum);
    fd = open(path, O_WRONLY); // disable 파일 열기
    if (fd == -1) {
        // 파일 열기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to open in disable!\n");
        return -1;
    }

    write(fd, s_disable_str, strlen(s_disable_str)); // disable 파일에 쓰기
    close(fd); // 파일 디스크립터 닫기

    return 0; // 성공적으로 비활성화 완료
}

// PWM 주기 설정 함수
static int PWMWritePeriod(int pwmnum, int value) {
    char s_value_str[VALUE_MAX]; // 값 문자열 버퍼
    char path[VALUE_MAX]; // 경로 버퍼
    int fd, byte; // 파일 디스크립터와 바이트 수

    // PWM period 파일 경로 설정
    snprintf(path, VALUE_MAX, "/sys/class/pwm/pwmchip0/pwm%d/period", pwmnum);
    fd = open(path, O_WRONLY); // period 파일 열기
    if (fd == -1) {
        // 파일 열기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to open in period!\n");
        return -1;
    }
    // 값 문자열 버퍼에 쓰기
    byte = snprintf(s_value_str, VALUE_MAX, "%d", value);

    // period 파일에 쓰기
    if (write(fd, s_value_str, byte) == -1) {
        // 값 쓰기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to write value in period!\n");
        close(fd);
        return -1;
    }
    close(fd); // 파일 디스크립터 닫기

    return 0; // 성공적으로 주기 설정 완료
}

// PWM 듀티 사이클 설정 함수
static int PWMWriteDutyCycle(int pwmnum, int value) {
    char s_value_str[VALUE_MAX]; // 값 문자열 버퍼
    char path[VALUE_MAX]; // 경로 버퍼
    int fd, byte; // 파일 디스크립터와 바이트 수

    // PWM duty_cycle 파일 경로 설정
    snprintf(path, VALUE_MAX, "/sys/class/pwm/pwmchip0/pwm%d/duty_cycle", pwmnum);
    fd = open(path, O_WRONLY); // duty_cycle 파일 열기
    if (fd == -1) {
        // 파일 열기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to open in duty cycle!\n");
        return -1;
    }
    // 값 문자열 버퍼에 쓰기
    byte = snprintf(s_value_str, VALUE_MAX, "%d", value);

    // duty_cycle 파일에 쓰기
    if (write(fd, s_value_str, byte) == -1) {
        // 값 쓰기 실패 시 오류 메시지 출력
        fprintf(stderr, "Failed to write value in duty cycle!\n");
        close(fd);
        return -1;
    }
    close(fd); // 파일 디스크립터 닫기

    return 0; // 성공적으로 듀티 사이클 설정 완료
}

/***************************************************************************
 * dispose_water(void *arg)
 * 쓰레드 cancel시 호출될 함수
 * LED, 부저를 끄고 연결된 GPIO핀, PWM서버를 제거함
 ***************************************************************************/
void dispose_water(void *arg) {
    // 할당한 자원 해제
    pthread_t *thread_id = (pthread_t*)arg; // 스레드 ID 포인터 가져오기

    // LED, 부저 끄기
    GPIOWrite(LED_PIN, LOW);
    GPIOWrite(BUZZER_PIN, LOW);

    // PWM 핀 unexport
    PWMUnexport(SERVO_PWM);
    
    // GPIO 핀 unexport
    GPIOUnexport(WATER_SUPPLY_PIN);
    GPIOUnexport(WATER_LEVEL_PIN);
    GPIOUnexport(LED_PIN);
    GPIOUnexport(BUZZER_PIN);

    free(thread_id); // 스레드 ID 메모리 해제
}

/***************************************************************************
 * set_servo_angle(int angle)
 * 물 공급 양 계산 함수
 * 온도, 습도와 반비례하게 물 공급 양을 계산함
 ***************************************************************************/
void set_servo_angle(int angle) {
    int pulse_width = (angle * 1000000 / 180) + 1000000; // 1ms ~ 2ms 펄스 폭
    int period = 20000000; // 20ms 주기

    PWMWritePeriod(SERVO_PWM, period); // PWM 주기 설정
    PWMWriteDutyCycle(SERVO_PWM, pulse_width); // PWM 듀티 사이클 설정
    PWMEnable(SERVO_PWM); // PWM 활성화
}

/***************************************************************************
 * cal_water_volume(float temp, float humid)
 * 물 공급 양 계산 함수
 * 온도, 습도와 반비례하게 물 공급 양을 계산함
 ***************************************************************************/
float cal_water_volume(float temp, float humid) {
    float volume = 0;
    volume += 10 - (temp / 10);
    volume += 10 - (humid / 10);
    return volume;
}

/***************************************************************************
 * water_control_thread(void* arg)
 * 물 공급 관리 스레드 함수
 * 실시간 온도, 습도에 맞게 서보모터가 동작할 시간을 계산
 * 물 부족이 인식된 상태라면 부저, LED 작동함
 * 물이 충분하거나 공급되면 LED 끄기
 ***************************************************************************/
void* water_control_thread(void* arg) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL); // 쓰레드 취소 가능 상태 설정
    pthread_cleanup_push(dispose_water, arg);

    float volume = cal_water_volume(temp, humid); // 온도와 습도에 따른 물의 양 계산
    int steps = (int)volume; // 계산된 물의 양을 기반으로 스텝 수 설정

    // 계산된 물의 양만큼 서보모터를 작동시킴
    for (int i = 0; i < steps; i++) {
        set_servo_angle(90); // 서보 모터를 90도 위치로 이동
        usleep(200000); // 0.2초 대기
        set_servo_angle(0); // 서보 모터를 0도 위치로 이동
        usleep(200000); // 0.2초 대기
    }

    // PWM 비활성화
    PWMDisable(SERVO_PWM);

    int status = 0;
    while (1) {
        if (GPIORead(WATER_LEVEL_PIN) == 0){
            if(status == 0){
                send(sockfd, "WATER LOW", strlen("WATER LOW"), 0); // 서버에 LED 켜짐 전송
                status = 1;
                // 물이 부족한 경우 LED와 부저 켜기
                // status flag로 처음 한번만 액추에이터 동작
                GPIOWrite(LED_PIN, HIGH);

                // 부저는 다음과 같이 작동하도록 함
                int melody[] = {262, 294, 330, 294, 262, 262, 262}; // 미레도레미미미 음계
                for (int i = 0; i < 7; i++) {
                    GPIOWrite(BUZZER_PIN, HIGH);
                    usleep(melody[i] * 1000);
                    GPIOWrite(BUZZER_PIN, LOW);
                    usleep(100000); // 음과 음 사이의 짧은 시간 대기
                }
            }
        } else {
            send(sockfd, "WATER OK", strlen("WATER OK"), 0);
            // 물이 충분한 경우 LED와 부저 끄기
            GPIOWrite(LED_PIN, LOW);
            GPIOWrite(BUZZER_PIN, LOW);
            break;
        }
        sleep(1);
    }

    pthread_cleanup_pop(1); // 쓰레드 종료 시 정리 함수 호출
    return NULL; // 스레드 종료
}

/***************************************************************************
 * init_water_control()
 * 물공급 관리 초기화 함수
 * 물공급 관리 스레드를 생성함
 ***************************************************************************/
pthread_t* init_water_control() {
    printf("Water management start\n"); // 초기화 메시지 출력
    pthread_t *thread_id = (pthread_t*)malloc(sizeof(pthread_t)); // 스레드 ID 메모리 할당
    if (thread_id == NULL) {
        fprintf(stderr, "Failed to allocate memory for thread ID\n");
        return NULL;
    }

    // GPIO 핀 내보내기
    if (GPIOExport(WATER_SUPPLY_PIN) == -1 || GPIOExport(WATER_LEVEL_PIN) == -1 ||
        GPIOExport(LED_PIN) == -1 || GPIOExport(BUZZER_PIN) == -1) {
        free(thread_id); // 실패 시 메모리 해제
        return NULL;
    }

    usleep(1000 * 200); // 설정 후 잠시 대기

    // GPIO 핀 방향 설정
    if (GPIODirection(WATER_SUPPLY_PIN, IN) == -1 || GPIODirection(WATER_LEVEL_PIN, IN) == -1 ||
        GPIODirection(LED_PIN, OUT) == -1 || GPIODirection(BUZZER_PIN, OUT) == -1) {
        free(thread_id); // 실패 시 메모리 해제
        return NULL;
    }

    // PWM 채널 내보내기
    if (PWMExport(SERVO_PWM) == -1) {
        free(thread_id); // 실패 시 메모리 해제
        return NULL;
    }

    usleep(1000 * 200); // PWM 설정 후 잠시 대기

    // 물 공급 관리 스레드 생성
    if (pthread_create(thread_id, NULL, water_control_thread, (void*)thread_id) != 0) {
        free(thread_id); // 실패 시 메모리 해제
        return NULL;
    }

    return thread_id; // 성공적으로 초기화 완료
}

/***************************************************************************
 * dispose_light(void *arg)
 * 쓰레드 cancel시 호출될 함수
 * LED를 끄고 연결된 GPIO핀을 제거함
 ***************************************************************************/
void dispose_light(void *arg) {
    // 할당한 자원 해제
    pthread_t *thread_id = (pthread_t*)arg; // 스레드 ID 포인터 가져오기

    // LED 끄기
    GPIOWrite(LED2_PIN, LOW);
    
    // GPIO 핀 unexport
    GPIOUnexport(LIGHT_SENSOR_PIN);
    GPIOUnexport(LED2_PIN);

    free(thread_id); // 스레드 ID 메모리 해제
}

/***************************************************************************
 * light_control_thread(void* arg)
 * 일조량 관리 스레드 함수
 * 조도 센서 값을 읽어와 LED를 작동함, LED 상태 변화 시 서버에 상태 전송
 ***************************************************************************/
void* light_control_thread(void* arg) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL); // 쓰레드 취소 가능 상태 설정
    pthread_cleanup_push(dispose_light, arg);

    int previous_status = !(GPIORead(LIGHT_SENSOR_PIN)); // 초기 상태 현재 센서 값과 반대로 설정
    
    while (1) {
        int light_value = GPIORead(LIGHT_SENSOR_PIN); // 일조량 센서 값 읽기
        if(previous_status != light_value) {
            if(light_value == 0) {
                GPIOWrite(LED2_PIN, HIGH); // LED 켜기
                send(sockfd, "LED ON", strlen("LED ON"), 0); // 서버에 LED 켜짐 전송
            } else {
                GPIOWrite(LED2_PIN, LOW); // LED 끄기
                send(sockfd, "LED OFF", strlen("LED OFF"), 0); // 서버에 LED 꺼짐 전송
            }
        }
        previous_status = light_value;
        sleep(0.2); // 0.2초마다 체크
    }
    pthread_cleanup_pop(1); // 쓰레드 종료 시 정리 함수 호출
    return NULL; // 스레드 종료
}

/***************************************************************************
 * init_light_control()
 * 일조량 관리 초기화 함수
 * 일조량 관리 스레드를 생성함
 ***************************************************************************/
pthread_t* init_light_control() {
    printf("Light management start\n");
    pthread_t *thread_id = (pthread_t*)malloc(sizeof(pthread_t)); // 스레드 ID 메모리 할당
    if (thread_id == NULL) {
        fprintf(stderr, "Failed to allocate memory for thread ID\n");
        return NULL;
    }

    // GPIO 핀 내보내기
    if (GPIOExport(LIGHT_SENSOR_PIN) == -1 || GPIOExport(LED2_PIN) == -1) {
        free(thread_id); // 실패 시 메모리 해제
        return NULL;
    }

    usleep(1000 * 200); // 설정 후 잠시 대기

    // GPIO 핀 방향 설정
    if (GPIODirection(LIGHT_SENSOR_PIN, IN) == -1 || GPIODirection(LED2_PIN, OUT) == -1) {
        free(thread_id); // 실패 시 메모리 해제
        return NULL;
    }

    // 일조량 관리 스레드 생성
    if (pthread_create(thread_id, NULL, light_control_thread, (void*)thread_id) != 0) {
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
    pthread_t *water_thread = NULL, *light_thread = NULL; // 스레드 포인터 선언

    // 무한 루프를 통해 계속해서 명령을 수신하고 처리
    while (1) {
        n = recv(sockfd, buffer, MAXLINE - 1, 0); // 소켓으로부터 데이터 수신
        if (n < 0) {
            perror("recv failed");
            break;
        }
        buffer[n] = '\0'; // 문자열 종료

        // 수신된 메시지에 따라 모드 설정 및 기능 실행
        if (strcmp(buffer, "WATER") == 0) {
            // 이전 스레드가 존재하면 종료하고 해제
            if (water_thread) {
                pthread_cancel(*water_thread); // 스레드 종료
                pthread_join(*water_thread, NULL);
                water_thread = NULL;
            }

            char response[MAXLINE];

            // 실시간 온도 받아옴
            request_and_receive("TEMP", response);
            temp = atoi(response) / 10;

            // 실시간 습도 받아옴
            request_and_receive("HUMID", response);
            humid = atoi(response) / 10;

            if (!water_thread) {
                water_thread = init_water_control();
                if (water_thread == NULL) {
                    printf("Failed to initialize water control\n"); // 실패 메시지 출력
                }
            }
        } else if (strcmp(buffer, "LIGHT_START") == 0) {
            if (!light_thread) {
                light_thread = init_light_control();
                if (light_thread == NULL) {
                    printf("Failed to initialize light control\n"); // 실패 메시지 출력
                }
            }
        } else if (strcmp(buffer, "LIGHT_END") == 0) {
            if (light_thread) {
                printf("Light management end\n");
                pthread_cancel(*light_thread); // 스레드 종료
                pthread_join(*light_thread, NULL);
                // free(light_thread);
                light_thread = NULL;
            }
        } else {
            fprintf(stderr, "Invalid command: %s\n", buffer);
        }
    }

    // 스레드 종료 대기 및 메모리 해제
    if (water_thread) {
        pthread_join(*water_thread, NULL);
        free(water_thread);
    }
    if (light_thread) {
        pthread_join(*light_thread, NULL);
        free(light_thread);
    }
}

/***************************************************************************
 * main(int argc, char *argv[]) 
 * 서버와 소켓 통신 연결
 ***************************************************************************/
int main(int argc, char *argv[]) {
    struct sockaddr_in serv_addr;
    char buffer[1024] = {0};

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

    // 소켓 통신을 통해 명령 수신 및 처리
    socket_communication();

    close(sockfd);

    return 0;
}