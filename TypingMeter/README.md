# TypingMeterについて<br>
<img width="1280" height="670" alt="image" src="https://github.com/user-attachments/assets/06a9aac7-6331-4f61-9088-c659603a23d9" />



TypingMeterは、PCと接続されたキーボードのタイピング速度や文字数を計測するアプリケーションです。<br>

**<主な機能>**
- **タイピング速度をスピードメーターのように表示**<br>
- **平均値や最大値をLOGとして可視化**<br>
- **ガソリンメーターを模したポモドーロタイマー機能**<br>
- **Night Drivingのアニメーションが見られるスクリーンセーバー機能**<br>
- **その他システムカラーの変更やバイブレーションのOn/off**<br>


**<基本構成>**
- **[Structure_1]**:自作キーボード[TheEndpoint](https://booth.pm/ja/items/5397024?srsltid=AfmBOopyI2rIjd3S4u9huHNX6b24YG9W6Mf1bb4Djlk5SMYoJj527o7B)(QMKFirmware・マイコン/RP2040)とと拡張基板TheExtendを介して、M5Core2とI2C通信で接続する<br>
- **[Structure_2]**:USBシリアル/Bluetooth及びRawHIDを使用し直接PCと接続し、M5Core2をスタンドアロンで活用する<br>

なお、Structure_2ではPCのローカル環境に専用アプリ[**typingbridge.py**](https://github.com/YMG-CODE/M5stack/tree/main/TypingBridge)をDLすることで、キーボードの入力情報をM5Core2へ送信します。

<img width="1039" height="582" alt="image" src="https://github.com/user-attachments/assets/a4b155ae-c753-415b-9e8e-1a116905916d" />
<img width="928" height="525" alt="image" src="https://github.com/user-attachments/assets/7ee7e73e-b908-4569-80b4-2992e57cb948" />

**<操作方法>**
- 起動後にモード選択
- - xxxx
