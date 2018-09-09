const int pwPin1 = 6;
long sensor;

void setup() {
  Serial.begin(57600);
  pinMode(5, OUTPUT);
  pinMode(pwPin1, INPUT);
}

void read_sensor () {
  digitalWrite(5, HIGH);
  delay(300);
  sensor = pulseIn(pwPin1, HIGH);
  digitalWrite(5, LOW);

}

void print_range() {
  Serial.print("S1");
  Serial.print("=");
  Serial.print( sensor);
  Serial.print(", ");
  Serial.println( (sensor * 1000L / 57874L));
}

void loop() {
  read_sensor();
  print_range();
  delay(100);
}
