# TheEndシリーズとは<br>

YMGWorksが設計した自作キーボードです。<br>
カラムスタッガード配列・60キーを前後を基本構成とした左右分割設計の基板を素体とし、<br>
拡張基板による複数デバイスの搭載/一部併用（トラックボール、トラックパッド、ロータリーエンコーダー、[AZ1UBALL](https://booth.pm/ja/items/4202085)他）、<br>
[M5StackCore2](https://docs.m5stack.com/ja/core/core2)搭載による、アプリケーション機能の追加、専用の一体型パームレストの搭載など、拡張性に富んでいる点が最大の特徴です。<br>



**<ビルドガイド一覧>**<br>
- **[Structure_1]**:自作キーボード[TheEndpoint](https://booth.pm/ja/items/5397024?srsltid=AfmBOopyI2rIjd3S4u9huHNX6b24YG9W6Mf1bb4Djlk5SMYoJj527o7B)(QMKFirmware・マイコン/RP2040)とと拡張基板TheExtendを介し、M5Core2とI2C通信で接続<br>
- **[Structure_2]**:USBシリアル/Bluetooth及びRawHIDを使用し直接PCと接続し、M5Core2をスタンドアロンで活用<br>

なお、Structure_2ではPCのローカル環境に専用アプリ[**typingbridge.py**](https://github.com/YMG-CODE/M5stack/tree/main/TypingBridge)をDLすることで、キーボードの入力情報をM5Core2へ送信します。<br><br>
各アプリケーションに関するマニュアル、利用規約、プライバシーポリシー等は、**各ディレクトリのREADME.md**に記載しておりますのでご一読頂きますようお願い致します。

<img width="931" height="568" alt="image" src="https://github.com/user-attachments/assets/19fb2edc-2877-4edf-92f7-c0078cdc7500" />
<img width="928" height="525" alt="image" src="https://github.com/user-attachments/assets/7ee7e73e-b908-4569-80b4-2992e57cb948" />





