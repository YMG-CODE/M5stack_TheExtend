# TheExtendについて<br>
<img width="930" height="526" alt="TheExtend" src="https://github.com/user-attachments/assets/bad5d9ba-cbc2-4483-b99f-740de1381e60" />


TheExtendは、M5StackCore2（ESP32）と自作キーボード(QMKFirmware/RP2040Zero)及びPCを接続して使用する拡張基板及び自作のアプリケーション群です。<br>

**<基本構成>**<br>
- **[Structure_1]**:自作キーボード[TheEndpoint](https://booth.pm/ja/items/5397024?srsltid=AfmBOopyI2rIjd3S4u9huHNX6b24YG9W6Mf1bb4Djlk5SMYoJj527o7B)(QMKFirmware・マイコン/RP2040)とと拡張基板TheExtendを介して、M5Core2とI2C通信で接続する<br>
- **[Structure_2]**:USBシリアル/Bluetooth及びRawHIDを使用し直接PCと接続し、M5Core2をスタンドアロンで活用する<br>

なお、Structure_2ではPCのローカル環境に専用アプリ[**typingbridge.py**](https://github.com/YMG-CODE/M5stack/tree/main/TypingBridge)をDLすることで、キーボードの入力情報をM5Core2へ送信します。

<img width="1039" height="582" alt="image" src="https://github.com/user-attachments/assets/a4b155ae-c753-415b-9e8e-1a116905916d" />
<img width="928" height="525" alt="image" src="https://github.com/user-attachments/assets/7ee7e73e-b908-4569-80b4-2992e57cb948" />
## typingbridge.pyについて<br>
typingbridgeはPCに接続されたキーボードの入力信号をM5Core2へ中継するハブとなるアプリケーションです。<br>USBシリアル通信/Bluetooth、RawHIDを用いての通信を成立させます。
本アプリケーションに関する利用規約、プライバシーポリシーは以下、ドキュメントに記載しておりますのでご一読頂きますようお願い致します。





