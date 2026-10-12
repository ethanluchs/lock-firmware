void setup() {
  pinMode(10, OUTPUT);
  pinMode(9, OUTPUT);
  pinMode(7, OUTPUT);
  pinMode(6, OUTPUT);
  pinMode(5, OUTPUT);
  pinMode(11, OUTPUT);
  pinMode(13, OUTPUT);
}

void loop() {
  digitalWrite(10, HIGH);
  digitalWrite(9, LOW);
  analogWrite(11, 150);

  digitalWrite(13, LOW);

  digitalWrite(7, HIGH);
  digitalWrite(6, LOW);
  analogWrite(5, 150);

  delay(2000);

  digitalWrite(13, HIGH);


  digitalWrite(10, LOW);
  digitalWrite(9, HIGH);
  analogWrite(11, 150);

  digitalWrite(13, LOW);

  digitalWrite(7, LOW);
  digitalWrite(6, HIGH);
  analogWrite(5, 150);

  digitalWrite(13, HIGH);


  delay(2000);
}