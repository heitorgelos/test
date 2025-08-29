#include <stdio.h>
#include "esp_err.h"
#include "driver/i2c.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

//Semáforo
    SemaphoreHandle_t sema = NULL;

//Definição das características globais
    #define NUM_SENS 8 //Numero de sensores
    bool save=false; //Contador de sensores lidos

//Struct
    //Struct das rodas com suas temperaturas e endereços
        typedef struct roda {
            uint8_t address;
            float temp;

        }Roda;

    //Criando o vetor de structs para representar as rodas
        Roda rodas[NUM_SENS]; //Var global com recurso administrado por semáforo

//I2C config

    //Definição de pinos do I2C
    #define sda_port  21 
    #define scl_port  22

    //Definição das características do mestre I2C
    #define pullup 1
    #define talk_freq 100000
    #define master_port 0
    #define buffer_tx 0
    #define buffer_rx 0
    #define addres_reg 0x07 // Registrador que armazena a temperatura objeto em cada um dos sensores


    //Configuração do mestre do barramento (este esp32) (pronto pra se comunicar após ela)
    void config_init_i2c(Roda *rodas){

        i2c_config_t setup = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = sda_port,
            .scl_io_num = scl_port,
            .sda_pullup_en = GPIO_PULLUP_ENABLE, //Verificar se é necessário utilizar
            .scl_pullup_en = GPIO_PULLUP_ENABLE, //Verificar se é necessário utilizar
            .master.clk_speed = talk_freq,
            .clk_flags = 0,  
        };

        i2c_param_config(master_port,&setup);

        i2c_driver_install(master_port,setup.mode,buffer_rx,buffer_tx,0);

        //Inicializando os endereços das structs das rodas com endereços de 7 bits
        uint8_t add=0x0A;
        for(int i=0;i<8;i++){
            rodas[i].address =add;
            add+=0x10;
        }    

    }

    //Função que faz a requisição do dado lido pelo sensor
    float i2c_request_tempC(uint8_t address_dev){

    uint8_t reg=addres_reg;
    uint16_t temp_bruta;
    float temp_celsius;
    uint8_t data[3]; //2 bytes de dados e 1 de crc (sensor tem conversor ADC de 17bits!)

     esp_err_t reading = i2c_master_write_read_device( //Read write chama sensor e pega dados instantâneamente
        master_port, //Porta do mestre
        address_dev, //Endereço do dispositivo
        &reg, //Endereço do registrador
        sizeof(reg),
        data, //Armazenamento de dados lidos
        3, //Numero de bytes lidos
        1000/portTICK_PERIOD_MS); //Frequência de leitura

        if(reading != ESP_OK){
            return 10000.00;
        }
    
        temp_bruta = (data[1]<<8)| data[0];
        temp_celsius = temp_bruta*0.02 -273.15; //Advinda do data sheet
        return temp_celsius;
}

//SPI config
    //Pinos para o SD
    #define MOSI 23
    #define MISO 19
    #define SCLK 18
    #define CS 5 

    //Função de inicialização do cartão SD 
    void config_init_spi_sd(){

        esp_err_t retr;
        //Configurando o barramento SPI
        sdmmc_host_t host = SDSPI_HOST_DEFAULT(); //Configurações default do host 
        host.slot = SPI3_HOST; //Os pinos selecionados utilizam esse barramento
        host.max_freq_khz = 2000; //Frequência máxima suportada 
        spi_bus_config_t bus_conf = {
            .mosi_io_num = MOSI, //Pino mosi
            .miso_io_num = MISO, //Pino miso
            .sclk_io_num = SCLK, //Pino clk
            .quadhd_io_num = -1, //Não usamos hold 
            .quadwp_io_num = -1, //Não usamos write protect
            .max_transfer_sz = 4000 //Número de máximo de bytes que podem ser transferidos 
        };

        //Inicializando barramento
        retr = spi_bus_initialize(host.slot,&bus_conf,SDSPI_DEFAULT_DMA); 
        if(retr!=ESP_OK){
            printf("Falha inicializando SPI! %s", esp_err_to_name(retr));
        }

        //Configurando o SD
        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = CS;
        slot_config.host_id = host.slot; //Indica qual host usar

        //Montando o sistema de arquivos
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false, //Se true formata sd se houver falha no FAT
            .max_files = 5, //Num max de arquivos a serem abertos no sd
            .allocation_unit_size = 16 * 512 //Tamanho do setor típico do cartão (esp idf recomenda pae)
        };

        //Montando o cartão e definindo o diretório de acesso do cartão como "/sdcard"
        sdmmc_card_t *card;
        retr = esp_vfs_fat_sdspi_mount("/sdcard", &host,&slot_config, &mount_config,&card); 
        
        if(retr!=ESP_OK){
            printf("Falha montando cartao! %s", esp_err_to_name(retr));
        }

        printf("Cartao montado!");
        sdmmc_card_print_info(stdout,card); //Mostrando infos do cartão pq informação é ouro ne pae
    }

//Tasks 

    //Task que lê dados 
    void read_temp(void *pvParameters){
        while (1)
        {
            if (xSemaphoreTake(sema,portMAX_DELAY)==pdTRUE)
            {
                for (int i = 0; i < NUM_SENS; i++)
                {
                    rodas[i].temp = i2c_request_tempC(rodas[i].address); //Pegando a temp de cada roda e salvando
                }
                save=true;
                xSemaphoreGive(sema);
            }

            vTaskDelay(pdMS_TO_TICKS(50)); //20 Hz 
            //Importante colocar um delay pq isso permite que o RTOS faça um escalonamento caso necessário
        }
        
    }

    //Task que salva dados
    void save_temp(void *pvParameters){
        FILE *arq=NULL;
        arq = fopen("/sdcard/log_temp.txt","a");
        if (arq==NULL)
        {
            ESP_LOGE("Save temp: ","Arquivo não aberto");
        }
        
        while (1)
        {   
            if (xSemaphoreTake(sema,portMAX_DELAY)==pdTRUE)
            {
                if (save) //Se todos sensores ja foram lidos, salvo os dados
                {
                    for (int i = 0; i < NUM_SENS; i++) //Flushing data ne pae
                    {
                        fprintf(arq, "Roda: 0x%02X - %.2f °C\n", rodas[i].address, rodas[i].temp);
                    }
                    fflush(arq);
                    
                    save=false; //Reinicio o index pra evitar que dados sejam reenviados
                }
                xSemaphoreGive(sema);
            }
            
            vTaskDelay(pdMS_TO_TICKS(50)); //20 Hz Delay 
            //Importante colocar um delay pq isso permite que o RTOS faça um escalonamento caso necessário
        }

        fclose(arq);
    }


void app_main(void)
{

    //Inicializar semáforo
    sema = xSemaphoreCreateMutex(); //Create mutex é a função correta para exclusão mútua no uso de recursos como variáveis
    //Com ela, a função que faz o "take" deve ser a mesma que faz o "give" e as de maior prioridade não esperam um give
    //xSemaphoreCreateBinary é mais recomendável para conciliar operação entre tasks, seu valor alterna entre 0 e 1
    if (sema==NULL)
    {
        ESP_LOGE("Main: ","Semaforo nao inicializado");
    }
    
    //xSemaphoreGive(sema); Não necessario em mutex

    //Configurando o i2c
    config_init_i2c(rodas);
   
    //Condigurando o SPI
    config_init_spi_sd();
   
    xTaskCreate(read_temp,"Read sensors",4096,rodas,5,NULL);
    xTaskCreate(save_temp,"Saving data",4096,rodas,5,NULL);
    
    
}
