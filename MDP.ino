#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "SDMMCBlockDevice.h"
#include "FATFileSystem.h"

WiFiClient wifiClient;                                    //와이파이 클라이언트를 MQTT 클라이언트에 전달
PubSubClient mqttClient(wifiClient);

char ssid[]="SCRC-Research";                              //공유기 SSID
char pass[]="kmuiout@2019";                               //공유기 비밀번호
char topic[100]="BIOLOGGER/PENGUIN/1/";                   //고정 토픽
const char* mqtt_server="192.168.50.191";                 //MQTT 서버 주소
String mqttClientID="SCRC_RFD-";                          //MQWTT 클라이언트 아이디
char* cutMessage;                                         //형식별로 자른 수신된 메세지 저장용

SDMMCBlockDevice block_device;                            //MBED 파일 시스템
mbed::FATFileSystem fs("fs");

char fileBuffer[1000];                                    //1줄에 1000바이트 이상시 누락
//char myFileName[]="fs/DataSample_small.csv";            //경로를 포함한 파일명 -현재 사용 중지
FILE *fp;                                                 //파일 선언
int number=1;                                             //파일 줄
int toggle=0;                                             //보내야할 파일 시작 줄
String message="";                                        //통합 메세지
String messageFile="";                                    //파일명
String messagePath="";                                    //경로포함 파일명
String messageByte="";                                    //파일 내용
String messageTopic=String(topic);                        //토픽명
String messageTemp="";                                    //임시 저장 메세지
String messageReady="";                                   //대기중 고정 메세지
char messageChar[1000];                                   //1000바이트씩 보내기 따라서 아이디+토픽명+파일명+줄번호+파일내용1줄이 1000바이트 이상시 누락

bool startSwitch=false;
bool stopSwitch=false;
bool restartSwitch=false;
bool doneSwitch=false;
bool continueSwitch=false;

DIR *dir;                                                 //폴더 선언
struct dirent *ent;                                       //폴더 읽기용 구조체
int dirIndex=0;                                           //읽은 파일 인덱스

void reconnect();                                         //함수 선언
void sendFile();
void sendByte();
void callback(char* topic,byte* payload,unsigned int length);

void setup(){
  Serial.begin(9600);                                     //아두이노
  
  pinMode(LED_BUILTIN,OUTPUT);                            //초록불

  Serial.println("BOOT");

  int err=fs.mount(&block_device);                        //SD카드 마운트
  if(err){                                                //SD카드 없을시
    Serial.println("NO SD CARD");
    Serial.println("RETRY...");
    err=fs.reformat(&block_device);                       //SD카드 재시도
  }
  if(err){                                                //SD카드 재시도 실패
    Serial.println("NO SD CARD");
    while(1){digitalWrite(LED_BUILTIN,!digitalRead(LED_BUILTIN));}
  }

  Serial.println("- LIST SD CARD CONTENT -");             //SD카드 내부 목록
  if((dir=opendir("/fs"))!=NULL){                         //SD카드 내부 목록 읽기 시도
    while((ent=readdir(dir))!=NULL){                      //읽기 성공시 하나씩 읽기
      Serial.println(ent->d_name);
      dirIndex++;
    }
    closedir(dir);
  }else{                                                  //SD카드 내부 목록 읽기 실패시
    Serial.println("ERROR OPENING SD CARD");
  }
  if(dirIndex==0){                                        //SD카드가 비어있을시
    Serial.println("EMPTY SD CARD");
  }
  
  while(WiFi.status()!=WL_CONNECTED){                     //WIFI연결시도
    Serial.println("CONNECTING WIFI");
    WiFi.begin(ssid,pass);
    delay(5000);                                          //5초 대기
  }
  Serial.println("WIFI CONNECTION COMPLETE");
  
  mqttClient.setServer(mqtt_server,1883);                 //MQTT 서버 세팅
  mqttClient.setCallback(callback);

  int checksum=0;                                         //고유 아이디 만들기용 체크섬 선언
  for(int u=0;u<2048;u++){
    checksum+=*((byte*)u);                                //checksum+=램 바이트 숫자
  }
  mqttClientID+=String(checksum,HEX);                     //고유 아이디 : SCRC_RFD-고유값
  Serial.print("MQTT CLIENT ID : ");
  Serial.println(mqttClientID);

  messageTopic+=mqttClientID;
  messageTopic.toCharArray(topic,sizeof(topic));

  if(mqttClient.connect(mqttClientID.c_str())){           //연결 성공시 MQTT CONNECTION COMPLETE 보내고 출력
    mqttClient.publish(topic,"MQTT CONNECTION COMPLETE");
    mqttClient.subscribe(mqttClientID.c_str());
    Serial.println("MQTT CONNECTION COMPLETE");
  }
}

void loop(){
  digitalWrite(LED_BUILTIN,HIGH);                         //초록불
  
  if (!mqttClient.connected()) {
    reconnect();                                          //MQTT 연결 시도
  }else{
    mqttClient.publish(topic,"START_READY");              //MQTT서버에 전송
    Serial.println("PUBLISHED MESSAGE : START_READY");
    mqttClient.loop();                                    //MQTT 메세지 수신
    if(startSwitch){
      sendFile();
      startSwitch=false;
    }
    delay(1000);                                          //1초 대기
  }
}

void sendFile(){
  while(mqttClient.connected()){                          //MQTT서버에 연결되어있다면 계속 보낸다.
    dirIndex=0;
    if((dir=opendir("/fs"))!=NULL){                       //SD카드 내부 목록 읽기 시도
      while((ent=readdir(dir))!=NULL){                    //읽기 성공시 하나씩 읽기
        if(strstr(ent->d_name,".")!=NULL){                //파일만 읽기 필터
          messageFile=String(ent->d_name);
          messagePath="fs/"+String(ent->d_name);          //fs/ 경로 합치기
          fp=fopen(messagePath.c_str(),"r");              //파일 열기
          if(fp==NULL){                                   //파일 없을시 넘기기
            Serial.println("NO FILE");
            continue;
          }
          number=1;                                       //줄번호 초기화
          toggle=0;                                       //보내야할 시작 줄 번호 초기화
          message="";                                     //메세지 초기화

          sendByte();

          dirIndex++;                                     //파일 인덱스 증가
          
          if(stopSwitch){
            stopSwitch=false;
            continue;
          }

          message=messageFile+":"+number+":DONE";         //파일명:줄번호:DONE -파일을 전부 보냈음을 알림
          message.toCharArray(messageChar,sizeof(messageChar));//CHAR로 변환

          mqttClient.publish(topic,messageChar);          //MQTT서버에 전송
          Serial.println("PUBLISHED DATA : ");            //보낸 내용 출력
          Serial.println(messageChar);

          fclose(fp);                                     //파일 닫기

          messageReady="DELETE_READY:"+messageFile;       //DELETE_READY:파일명 -삭제 대기 메세지

          continueSwitch=false;
          doneSwitch=true;
          while(1){
            mqttClient.loop();                            //MQTT 메세지 수신
            if(continueSwitch){
              break;
            }else if(restartSwitch){
              restartSwitch=false;
              fp=fopen(messagePath.c_str(),"r");          //파일 열기
              if(fp==NULL){                               //파일 없을시 넘기기
                Serial.println("NO FILE");
                mqttClient.publish(topic,"NO FILE");
                break;
              }else{
                number=1;                                 //줄번호 초기화
                message="";                               //메세지 초기화
                stopSwitch=false;
                sendByte();
                fclose(fp);
                break;
              }
            }
            mqttClient.publish(topic,messageReady.c_str());//MQTT서버에 전송
            Serial.print("PUBLISHED MESSAGE : ");
            Serial.print(messageReady);
            delay(1000);                                  //1초 대기
          }
          doneSwitch=false;
        }
      }
      closedir(dir);
    }else{                                                //SD카드 내부 목록 읽기 실패시
      Serial.println("ERROR OPENING SD CARD");
      mqttClient.publish(topic,"ERROR OPENING SD CARD");  //MQTT서버에 전송
    }
    if(dirIndex==0){                                      //SD카드가 비어있을시
      Serial.println("EMPTY SD CARD");
      mqttClient.publish(topic,"EMPTY SD CARD");          //MQTT서버에 전송
    }
    delay(5000);                                          //5초 대기
  }
}

void sendByte(){
  while(1) {                                              //파일 하나를 다 보낼때까지 무한 반복
    fgets(fileBuffer,sizeof(fileBuffer),fp);
    if(feof(fp)){break;}                                  //파일 하나를 다 읽었으면 반복문 탈출

    if(number>=toggle){
      messageByte=String(fileBuffer);
      message=messageFile+":"+number+":"+messageByte;     //파일명:줄번호:파일내용
      message.toCharArray(messageChar,sizeof(messageChar));//CHAR로 변환
        
      mqttClient.publish(topic,messageChar);              //MQTT서버에 전송
      Serial.println("PUBLISHED DATA : ");                //보낸 내용 출력
      Serial.println(messageChar);
    }

    number++;                                             //줄번호 증가

    mqttClient.loop();

    if(stopSwitch){
      while(1){
        if(restartSwitch){
          restartSwitch=false;
          break;
        }
        message="OK:"+messageFile;
        message.toCharArray(messageChar,sizeof(messageChar));//CHAR로 변환
        mqttClient.publish(topic,messageChar);            //MQTT서버에 전송
        Serial.print("PUBLISHED MESSAGE : ");
        Serial.println(messageChar);
        mqttClient.loop();
        delay(1000);                                      //1초 대기
      }
      fclose(fp);
      fp=fopen(messagePath.c_str(),"r");                  //파일 열기
      if(fp==NULL){                                       //파일 없을시 넘기기
        Serial.println("NO FILE");
        mqttClient.publish(topic,"NO FILE");
        break;
      }else{
        number=1;                                         //줄번호 초기화
        message="";                                       //메세지 초기화
        stopSwitch=false;
        continue;
      }
    }
  }
}

void reconnect(){
  while (WiFi.status()!=WL_CONNECTED) {                   //WIFI가 문제일수도 있으므로 WIFI가 끊겨있을시 연결될때까지 반복
    Serial.println("CONNECTING WIFI");
    WiFi.begin(ssid,pass);
    delay(5000);                                          //5초 대기
  }
  Serial.println("WIFI CHECK COMPLETE");
  
  while(!mqttClient.connected()){                         //MQTT서버에 연결될 때까지 반복
    Serial.println("CONNECTING MQTT");
//    String mqttClientID="SCRC_RFD-";                    //랜덤 아이디 생성 -현재 사용 중지
//    mqttClientID+=String(random(0xffff),HEX);
    
    if(mqttClient.connect(mqttClientID.c_str())){         //연결 성공시 MQTT CONNECTION COMPLETE 보내고 출력
      mqttClient.publish(topic,"MQTT CONNECTION COMPLETE");
      mqttClient.subscribe(mqttClientID.c_str());
      Serial.println("MQTT CONNECTION COMPLETE");
    }else{                                                //연결 실패시 현상태 출력
      Serial.print("FAILED : ");
      Serial.println(mqttClient.state());
      Serial.println("TRY AGAIN IN 5 SECONDS");
      delay(5000);                                        //5초 대기
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length){//메세지 수신
  Serial.print("CONTROL MESSAGE : ");
  message="";                                             //메세지 초가화
  for (int i = 0; i < length; i++){
    message+=String((char)payload[i]);
  }
  Serial.println(message);                                //수신 메세지 출력
  message.toCharArray(messageChar,sizeof(messageChar));   //CHAR로 변환
  cutMessage=strtok(messageChar,":");                     //:로 구분해서 명령어 나누기
  if(cutMessage!=NULL){
    if(strcmp(cutMessage,"START")==0){
      startSwitch=true;
      return;
    }else if(strcmp(cutMessage,"STOP")==0){
      stopSwitch=true;
      return;
    }else if(strcmp(cutMessage,"RESTART")==0){
      if(!(stopSwitch||doneSwitch)){return;}
      cutMessage=strtok(NULL,":");                        //:로 구분된 명령어의 다음 구문
      if(cutMessage!=NULL){
        messageTemp=String(cutMessage);
        cutMessage=strtok(NULL,":");                      //:로 구분된 명령어의 다음 구문
        if(cutMessage!=NULL){
          toggle=atoi(cutMessage);
          if(toggle==0){
            Serial.println("INCORRECT FILE LINE");
            mqttClient.publish(topic,"INCORRECT FILE LINE");
            continueSwitch=true;
            return;
          }
          messageFile=messageTemp;
          messagePath="fs/"+messageTemp;                  //경로 fs/ 붙이기
          restartSwitch=true;
          return;
        }else{                                            //파일명 뒤에 아무것도 없을시
          Serial.println("NO FILE LINE");
          mqttClient.publish(topic,"NO FILE LINE");
          continueSwitch=true;
          return;
        }
      }else{                                              //RESTART 명령어 뒤에 아무것도 없을시
        Serial.println("INCORRECT RESTART MESSAGE");
        mqttClient.publish(topic,"INCORRECT RESTART MESSAGE");//MQTT서버에 전송
        continueSwitch=true;
        return;
      }
    }else if(strcmp(cutMessage,"DELETE")==0){             //DELETE 명렁어 수신시
      if(!doneSwitch){return;}
      continueSwitch=true;
      cutMessage=strtok(NULL,":");                        //:로 구분된 명령어의 다음 구문
      if(cutMessage!=NULL){
        messageFile=String(cutMessage);
        messagePath="fs/"+String(cutMessage);             //경로 fs/ 붙이기
        number=remove(messagePath.c_str());               //파일 삭제
        if(number==0){                                    //파일 삭제 성공시
          Serial.print(messagePath);
          Serial.println(" : DELETE COMPLETE");
          message=messageFile+" - DELETE COMPLETE";       //파일명 - DELETE COMPLETE
          message.toCharArray(messageChar,sizeof(messageChar));//CHAR로 변환
          mqttClient.publish(topic,messageChar);          //MQTT서버에 전송
          return;
        }else{                                            //파일 삭제 실패시
          Serial.print(messagePath);
          Serial.println(" : DELETE FAIL");
          message=messageFile+" - DELETE FAIL";           //파일명 - DELETE FAIL
          message.toCharArray(messageChar,sizeof(messageChar));//CHAR로 변환
          mqttClient.publish(topic,messageChar);          //MQTT서버에 전송
          return;
        }
      }else{                                              //DELETE 명령어 뒤에 아무것도 없을시
        Serial.println("INCORRECT DELETE MESSAGE");
        mqttClient.publish(topic,"INCORRECT DELETE MESSAGE");//MQTT서버에 전송
        return;
      }
    }else{                                                //잘못된 형식의 명령어 수신시
      Serial.println("INCORRECT CONTROL MESSAGE");
      mqttClient.publish(topic,"INCORRECT CONTROL MESSAGE");//MQTT서버에 전송
      return;
    }
  }else{
    Serial.println("NO CONTROL MESSAGE");
    mqttClient.publish(topic,"NO CONTROL MESSAGE");       //MQTT서버에 전송
    return;
  }
}
