# Tanque_Cisterna_Web_Control_ESP32
Con una esp32 dos sensores ultrasonicos y un relay manejamos el motor de una bomba y vemos el estado del tanque en tiempo real en un servidor web hosteado en la misma ESP32

Setup

Una ESP32 yo use una NodeMCU 32S creo...
Dos sendores ultrasonicos para la cisterna y el tanque
Un relay de 5v 10amp 220v o la tension que manejen en su lugar

Conectan la ESP32 al usb o lo que sea que usen para comunicarse

Los drivers usb que necesiten (la IA es su amiga)


1. Instalan y abren **[Arduino IDE](https://docs.arduino.cc/software/ide/)**.
2. Si no consiguen comunicarse con la Esp32 instalan el driver **[WCH CH340 Driver](https://www.tecneu.com/blogs/tutoriales-de-electronica/guia-paso-a-paso-para-instalar-el-driver-ch340g-en-windows-y-mac?srsltid=AfmBOorWYHYStt433QoR3n1FMt2kGe9WqyiUDfcd9x9y8EoXX6WNAXF_)** o el **[Silicon Labs CP210x](https://community.silabs.com/s/question/0D58Y00008K88dCSAR/how-to-download-cp210x-usb-to-uart-bridge-vcp-drivers?language=es)**
3. Editan el ssid y la password de su wifi
  
    // ⚠️ CAMBIA ESTO CON LOS DATOS DE TU CASA ⚠️
   
    const char* ssid = "EL_NOMBRE_DE_TU_WIFI";
   
    const char* password = "TU_CONTRASEÑA_DEL_WIFI";

5. **[En el simulador pueden probar el codigo](https://wokwi.com/projects/467575637641711617)** y los sensores, tiene la opcion de variar los parametros tocando los sensores
6. **[Pegan el codigo](https://github.com/yimgame/Tanque_Cisterna_Web_Control_ESP32/blob/main/sketch/sketch.ino)**, compilan una prueba y lo suben a la ESP32 si da bien



Una vez compilado en la ESP32 te da la ip para conectarte, lo normal es que la asigne el dhcp de tu router


El projecto se encuentra simulado en wolwi


https://wokwi.com/projects/467575637641711617


Codifico: Gemini


Guia espiritual: Yim


Just coding 4 fun !!!


https://gemini.google.com/app/cda3b56294498d7e


![alt text](image.png)

![alt text](image-1.png)

![alt text](image-2.png)
