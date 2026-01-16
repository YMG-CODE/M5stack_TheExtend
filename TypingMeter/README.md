# TypingMeterについて<br>
<img width="1280" height="670" alt="image" src="https://github.com/user-attachments/assets/06a9aac7-6331-4f61-9088-c659603a23d9" />



TypingMeterは、PCと接続されたキーボードのタイピング速度や文字数を計測するアプリケーションです。<br>

**<主な機能>**
- **タイピング速度をスピードメーターのように表示**<br>
- **平均値や最大値をLOGとして可視化**<br>
- **ガソリンメーターを模したポモドーロタイマー機能**<br>
- **Night Drivingのアニメーションが見られるスクリーンセーバー機能**<br>
- **その他システムカラーの変更やバイブレーションのOn/off**<br>


基本構成として自作キーボードとI2C通信を行うStructure_1とPCを介してUSBシリアル/Bluetooth及びRawHIDを使用するStructure_2があります。<br>
Structure_2ではPCのローカル環境に専用アプリtypingbridge.pyをインストールすることで、キーボードの入力情報をM5Core2へ送信しています。

<img width="1039" height="582" alt="image" src="https://github.com/user-attachments/assets/a4b155ae-c753-415b-9e8e-1a116905916d" />
<img width="928" height="525" alt="image" src="https://github.com/user-attachments/assets/7ee7e73e-b908-4569-80b4-2992e57cb948" />

## TheExtendについて<br>
typingbridgeはPCに接続されたキーボードの入力信号をM5Core2へ中継するハブとなるアプリケーションです。USBシリアル通信/Bluetooth、RawHIDを用いての通信を成立させます。
本アプリケーションに関する利用規約、プライバシーポリシーは以下、ドキュメントに記載しておりますのでご一読頂きますようお願い致します。
