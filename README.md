# TheExtendについて<br>

TheExtendは、M5StackCore2（ESP32）と自作キーボード(QMKFirmware/RP2040Zero)及びPCを接続して使用する拡張基板及び自作のアプリケーションです。<br>

基本構成として自作キーボードとI2C通信を行うStructure_1とPCを介してUSBシリアル及びRawHIDを使用するStructure_2があります。<br>

Structure_2ではPCのローカル環境に専用アプリtypingbridge.pyをインストールすることで、キーボードの入力情報をM5Core2へ送信しています。

<img width="1039" height="582" alt="image" src="https://github.com/user-attachments/assets/a4b155ae-c753-415b-9e8e-1a116905916d" />
<img width="994" height="506" alt="image" src="https://github.com/user-attachments/assets/fed7ffcb-4492-4140-a39a-9b2fdf05d387" />
<br>
### typingbridge.pyについて
typingbridgeはPCに接続されたキーボードの入力信号をM5Core2へ中継するハブとなるアプリケーションです。USBシリアル通信/Bluetooth、RawHIDを用いての通信を成立させます。
本アプリケーションに関する利用規約、プライバシーポリシーは以下、ドキュメントに記載しておりますのでご一読頂きますようお願い致します。





