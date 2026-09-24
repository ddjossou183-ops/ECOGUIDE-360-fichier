// ============================================================
// ECOGUIDE 360° - Station intelligente de tri (ESP32)
// Version 5 : niveaux forcés à vide, clapet ouvert plus longtemps,
//             un seul cycle par personne, nouveau cycle possible après le départ
//             de la personne (ou après DELAI_REARMEMENT_MS)
// ============================================================
//
// CÂBLAGE À MODIFIER PAR RAPPORT À LA VERSION PRÉCÉDENTE
//   TRIG papier    : GPIO12 -> GPIO16   (GPIO12 = broche de démarrage)
//   ECHO papier    : GPIO15 -> GPIO34   (GPIO15 = broche de démarrage)
//   Buzzer         : GPIO2  -> GPIO17   (GPIO2  = broche de démarrage)
//
// MATÉRIEL OBLIGATOIRE
//   - Servos alimentés par une source 5 V séparée (2 A mini),
//     GND commun avec l'ESP32, condensateur 470-1000 µF sur le 5 V servos.
//   - Chaque broche ECHO des HC-SR04 passe par un pont diviseur
//     (1 kΩ vers la broche ESP32 + 2 kΩ vers GND) : l'ESP32 est en 3,3 V.
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>


// ============================================================
// CONFIGURATION WIFI (2,4 GHz uniquement)
// ============================================================

const char* WIFI_SSID     = "S100";
const char* WIFI_PASSWORD = "DJOSSOU12";


// ============================================================
// SERVEUR NETLIFY  -->  À REMPLACER PAR TES VRAIES VALEURS
// ============================================================

const char* SERVER_URL     = "https://VOTRE-SITE.netlify.app/api/measurements";
const char* DEVICE_API_KEY = "VOTRE_DEVICE_API_KEY";


// ============================================================
// IDENTIFICATION DE LA STATION
// ============================================================

const char* DEVICE_CODE   = "ECOGUIDE-001";
const char* SITE_NAME     = "ECOGUIDE 360°";
const char* LOCATION_TEXT = "Cotonou - Benin";


// ============================================================
// LCD 16x2 I2C  (adresse 0x27 ou 0x3F : voir le scan I2C au démarrage)
// ============================================================

LiquidCrystal_I2C lcd(0x27, 16, 2);
const int I2C_SDA = 21;
const int I2C_SCL = 22;


// ============================================================
// BROCHES
// ============================================================

// HC-SR04
const int TRIG_PRESENCE  = 4;
const int ECHO_PRESENCE  = 18;

const int TRIG_PLASTIQUE = 5;
const int ECHO_PLASTIQUE = 19;

const int TRIG_PAPIER    = 12;   // avant : 12
const int ECHO_PAPIER    = 15;   // avant : 15 (34 = entrée seule, OK pour un écho)

const int TRIG_ORGANIQUE = 27;
const int ECHO_ORGANIQUE = 35;   // entrée seule, OK pour un écho

// Boutons (câblés entre la broche et GND)
const int BTN_PLASTIQUE = 25;
const int BTN_PAPIER    = 26;
const int BTN_ORGANIQUE = 32;

// Servos
const int SERVO_PLASTIQUE_PIN = 13;
const int SERVO_PAPIER_PIN    = 14;
const int SERVO_ORGANIQUE_PIN = 33;

// LED (état serveur) et buzzer
const int LED_PIN    = 23;
const int BUZZER_PIN = 2;       // avant : 2

// true  = buzzer actif qui sonne quand la broche est HIGH
// false = module buzzer "déclenchement niveau bas" (sonne quand LOW)
const bool BUZZER_ACTIF_A_HIGH = true;


// ============================================================
// SERVOS
// ============================================================

Servo servoPlastique;
Servo servoPapier;
Servo servoOrganique;

const int ANGLE_FERME  = 0;
const int ANGLE_OUVERT = 90;

const int SERVO_US_MIN = 500;
const int SERVO_US_MAX = 2400;


// ============================================================
// CALIBRATION DES COMPARTIMENTS
// ============================================================

const float EMPTY_DISTANCE_CM = 35.0;   // capteur -> fond, bac vide
const float FULL_DISTANCE_CM  = 7.0;    // capteur -> déchets, bac plein

// true  = tous les compartiments sont affichés/envoyés à 0 % (vides)
// false = niveaux réels lus par les capteurs ultrasoniques
const bool NIVEAUX_FORCES_VIDES = true;

const int SEUIL_ALERTE   = 80;          // alerte affichée à l'écran
const int SEUIL_CRITIQUE = 90;          // compartiment refusé


// ============================================================
// PRÉSENCE ET TEMPS
// ============================================================

const float DISTANCE_PRESENCE_CM = 100.0;

const unsigned long INTERVALLE_ENVOI  = 30000;  // mesure + envoi périodiques
const unsigned long DELAI_PRESENCE    = 5000;   // pause entre deux sessions
const unsigned long DELAI_REARMEMENT_MS = 4000; // nouveau cycle possible même si quelqu'un reste devant
const bool DEBUG_PRESENCE = true;               // affiche la distance de présence dans le moniteur série
const unsigned long TEMPS_CHOIX_MS    = 12000;  // temps pour choisir
const unsigned long TEMPS_DEPOT_MS    = 3500;   // clapet ouvert (avant : 2500)


// ============================================================
// VARIABLES
// ============================================================

int niveauPlastique = 0;
int niveauPapier    = 0;
int niveauOrganique = 0;

bool presence   = false;
float distancePresenceCm = -1;
bool serveurOK  = false;

unsigned long dernierEnvoi      = 0;
unsigned long dernierePresence  = 0;


// ============================================================
// LCD
// ============================================================

void afficherMessage(String ligne1, String ligne2 = "")
{
  lcd.setCursor(0, 0);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print("                ");

  lcd.setCursor(0, 0);
  lcd.print(ligne1.substring(0, 16));

  lcd.setCursor(0, 1);
  lcd.print(ligne2.substring(0, 16));
}


// ============================================================
// BUZZER
// ============================================================

void buzzerEteint()
{
  digitalWrite(BUZZER_PIN, BUZZER_ACTIF_A_HIGH ? LOW : HIGH);
}

void bip(int duree = 100)
{
  digitalWrite(BUZZER_PIN, BUZZER_ACTIF_A_HIGH ? HIGH : LOW);
  delay(duree);
  buzzerEteint();
}

void bipOK()
{
  bip(80);
  delay(70);
  bip(80);
}

void bipErreur()
{
  for (int i = 0; i < 3; i++)
  {
    bip(70);
    delay(60);
  }
}


// ============================================================
// MESURE HC-SR04
// ============================================================

// Une seule mesure brute. Retourne -1 si pas d'écho.
float mesurerDistanceBrute(int trigPin, int echoPin)
{
  digitalWrite(trigPin, LOW);
  delayMicroseconds(3);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duree = pulseIn(echoPin, HIGH, 30000UL);

  if (duree == 0)
  {
    return -1;
  }

  return duree * 0.0343 / 2.0;
}


// Médiane de plusieurs mesures : élimine les valeurs parasites.
// Retourne -1 si aucune mesure valide.
float mesurerDistance(int trigPin, int echoPin, int nombre = 3)
{
  float valeurs[5];
  int n = 0;

  if (nombre > 5)
  {
    nombre = 5;
  }

  for (int i = 0; i < nombre; i++)
  {
    float d = mesurerDistanceBrute(trigPin, echoPin);

    if (d > 0)
    {
      valeurs[n] = d;
      n++;
    }

    if (i < nombre - 1)
    {
      delay(30);
    }
  }

  if (n == 0)
  {
    return -1;
  }

  // Tri par insertion, puis médiane.
  for (int i = 1; i < n; i++)
  {
    float cle = valeurs[i];
    int j = i - 1;

    while (j >= 0 && valeurs[j] > cle)
    {
      valeurs[j + 1] = valeurs[j];
      j--;
    }

    valeurs[j + 1] = cle;
  }

  return valeurs[n / 2];
}


// ============================================================
// DISTANCE -> POURCENTAGE
// ============================================================

int convertirEnPourcentage(float distance)
{
  if (distance < 0)
  {
    return -1;
  }

  float pourcentage =
    ((EMPTY_DISTANCE_CM - distance) /
     (EMPTY_DISTANCE_CM - FULL_DISTANCE_CM)) * 100.0;

  if (pourcentage < 0)   pourcentage = 0;
  if (pourcentage > 100) pourcentage = 100;

  return (int)round(pourcentage);
}


// ============================================================
// LECTURE DES 3 NIVEAUX
// ============================================================

void mesurerNiveaux()
{
  float dPlastique = mesurerDistance(TRIG_PLASTIQUE, ECHO_PLASTIQUE);
  delay(40);

  float dPapier = mesurerDistance(TRIG_PAPIER, ECHO_PAPIER);
  delay(40);

  float dOrganique = mesurerDistance(TRIG_ORGANIQUE, ECHO_ORGANIQUE);

  int plastique = convertirEnPourcentage(dPlastique);
  int papier    = convertirEnPourcentage(dPapier);
  int organique = convertirEnPourcentage(dOrganique);

  // -1 = capteur muet : on garde la dernière valeur connue.
  if (plastique >= 0) niveauPlastique = plastique;
  if (papier    >= 0) niveauPapier    = papier;
  if (organique >= 0) niveauOrganique = organique;

  // Compartiments forcés à vide (les distances réelles restent visibles dans le moniteur série)
  if (NIVEAUX_FORCES_VIDES)
  {
    niveauPlastique = 0;
    niveauPapier    = 0;
    niveauOrganique = 0;
  }

  Serial.println("---------- NIVEAUX ----------");
  Serial.printf("Plastique : %.1f cm -> %d %%%s\n", dPlastique, niveauPlastique, plastique < 0 ? "  (CAPTEUR SANS ECHO)" : "");
  Serial.printf("Papier    : %.1f cm -> %d %%%s\n", dPapier,    niveauPapier,    papier    < 0 ? "  (CAPTEUR SANS ECHO)" : "");
  Serial.printf("Organique : %.1f cm -> %d %%%s\n", dOrganique, niveauOrganique, organique < 0 ? "  (CAPTEUR SANS ECHO)" : "");
  if (NIVEAUX_FORCES_VIDES)
  {
    Serial.println("(niveaux forces a 0 % : NIVEAUX_FORCES_VIDES = true)");
  }

  Serial.println("-----------------------------");
}


bool compartimentCritique(int niveau)
{
  return niveau >= SEUIL_CRITIQUE;
}

bool uneAlerteActive()
{
  return niveauPlastique >= SEUIL_ALERTE ||
         niveauPapier    >= SEUIL_ALERTE ||
         niveauOrganique >= SEUIL_ALERTE;
}


// ============================================================
// DÉTECTION DE PRÉSENCE
// ============================================================

bool detecterPresence()
{
  distancePresenceCm = mesurerDistanceBrute(TRIG_PRESENCE, ECHO_PRESENCE);

  return (distancePresenceCm > 0 && distancePresenceCm <= DISTANCE_PRESENCE_CM);
}


// ============================================================
// SERVOS
// ============================================================

void fermerTousLesClapets()
{
  servoPlastique.write(ANGLE_FERME);
  servoPapier.write(ANGLE_FERME);
  servoOrganique.write(ANGLE_FERME);
}


// ============================================================
// AFFICHAGES
// ============================================================

void afficherNiveaux()
{
  afficherMessage(
    "Pl:" + String(niveauPlastique) + "% Pa:" + String(niveauPapier) + "%",
    "Or:" + String(niveauOrganique) + "%"
  );

  delay(1800);
}

void afficherChoix()
{
  afficherMessage("Choisissez :", "Plast Papier Org");
}


// ============================================================
// DIAGNOSTIC I2C
// ============================================================

void scannerI2C()
{
  Serial.println("Scan I2C...");

  int trouves = 0;

  for (byte adresse = 1; adresse < 127; adresse++)
  {
    Wire.beginTransmission(adresse);

    if (Wire.endTransmission() == 0)
    {
      Serial.print("  Peripherique en 0x");
      Serial.println(adresse, HEX);
      trouves++;
    }
  }

  if (trouves == 0)
  {
    Serial.println("  AUCUN peripherique I2C : verifier SDA/SCL et l'alimentation du LCD");
  }
}


// ============================================================
// WIFI
// ============================================================

void connecterWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  afficherMessage("Connexion WiFi", "Patientez...");

  int essais = 0;

  while (WiFi.status() != WL_CONNECTED && essais < 30)
  {
    delay(500);
    essais++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("WiFi OK, IP : ");
    Serial.println(WiFi.localIP());

    afficherMessage("WiFi OK", WiFi.localIP().toString());
  }
  else
  {
    Serial.println("WiFi indisponible : mode local (nouvelle tentative automatique)");

    afficherMessage("WiFi absent", "Mode local");
  }

  delay(1200);
}


// Reconnexion sans bloquer : essai toutes les 15 s si le WiFi est perdu.
void verifierWiFi()
{
  static unsigned long dernierEssai = 0;

  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }

  if (millis() - dernierEssai < 15000)
  {
    return;
  }

  dernierEssai = millis();

  Serial.println("WiFi perdu : nouvelle tentative...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}


// ============================================================
// ENVOI DES DONNÉES À NETLIFY
// ============================================================

bool configServeurValide()
{
  return strstr(SERVER_URL, "VOTRE-SITE") == NULL &&
         strstr(DEVICE_API_KEY, "VOTRE_") == NULL;
}


void envoyerDonnees()
{
  dernierEnvoi = millis();

  if (WiFi.status() != WL_CONNECTED)
  {
    serveurOK = false;
    return;
  }

  if (!configServeurValide())
  {
    static bool dejaSignale = false;

    if (!dejaSignale)
    {
      Serial.println("!! SERVER_URL / DEVICE_API_KEY non renseignes : aucun envoi possible");
      dejaSignale = true;
    }

    serveurOK = false;
    return;
  }

  StaticJsonDocument<512> document;

  document["device_code"]   = DEVICE_CODE;
  document["site_name"]     = SITE_NAME;
  document["location_text"] = LOCATION_TEXT;
  document["plastique"]     = niveauPlastique;
  document["papier"]        = niveauPapier;
  document["organique"]     = niveauOrganique;
  document["presence"]      = presence;

  String contenuJSON;
  serializeJson(document, contenuJSON);

  WiFiClientSecure client;
  client.setInsecure();   // prototype : à remplacer par un certificat CA en production

  HTTPClient http;

  if (!http.begin(client, SERVER_URL))
  {
    serveurOK = false;
    return;
  }

  http.setConnectTimeout(4000);
  http.setTimeout(4000);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-key", DEVICE_API_KEY);

  int codeHTTP = http.POST(contenuJSON);

  Serial.println("========== ENVOI ==========");
  Serial.print("HTTP : ");
  Serial.println(codeHTTP);
  Serial.print("JSON : ");
  Serial.println(contenuJSON);

  if (codeHTTP < 0)
  {
    Serial.print("Erreur : ");
    Serial.println(HTTPClient::errorToString(codeHTTP));
  }
  else if (codeHTTP < 200 || codeHTTP >= 300)
  {
    Serial.print("Reponse serveur : ");
    Serial.println(http.getString());
  }

  Serial.println("===========================");

  serveurOK = (codeHTTP >= 200 && codeHTTP < 300);

  http.end();
}


// ============================================================
// TRI D'UN DÉCHET (une seule fonction pour les 3 compartiments)
// Retourne true si le dépôt a eu lieu, false si compartiment plein.
// ============================================================

bool trierCompartiment(const char* nom, Servo &servo, int niveau)
{
  if (compartimentCritique(niveau))
  {
    afficherMessage(String(nom) + " PLEIN", "Choisir autre");
    bipErreur();
    delay(1500);

    return false;
  }

  afficherMessage(nom, "Ouverture...");
  bipOK();

  servo.write(ANGLE_OUVERT);
  delay(1200);

  afficherMessage("Deposez le", "dechet");
  delay(TEMPS_DEPOT_MS);

  servo.write(ANGLE_FERME);

  afficherMessage("Fermeture", "Mesure...");
  delay(1000);   // laisse le clapet se fermer et les déchets se stabiliser

  mesurerNiveaux();
  envoyerDonnees();
  afficherNiveaux();

  return true;
}


// ============================================================
// BOUTONS
// ============================================================

bool boutonAppuye(int pin)
{
  if (digitalRead(pin) == LOW)
  {
    delay(40);

    if (digitalRead(pin) == LOW)
    {
      return true;
    }
  }

  return false;
}


// ============================================================
// GESTION DU CHOIX UTILISATEUR
// ============================================================

void gererTri()
{
  mesurerNiveaux();

  afficherChoix();

  unsigned long debut = millis();

  while (millis() - debut < TEMPS_CHOIX_MS)
  {
    if (boutonAppuye(BTN_PLASTIQUE))
    {
      if (trierCompartiment("PLASTIQUE", servoPlastique, niveauPlastique))
      {
        return;
      }

      afficherChoix();
    }

    if (boutonAppuye(BTN_PAPIER))
    {
      if (trierCompartiment("PAPIER", servoPapier, niveauPapier))
      {
        return;
      }

      afficherChoix();
    }

    if (boutonAppuye(BTN_ORGANIQUE))
    {
      if (trierCompartiment("ORGANIQUE", servoOrganique, niveauOrganique))
      {
        return;
      }

      afficherChoix();
    }

    delay(20);
  }

  afficherMessage("Aucun choix", "Merci");
  delay(1000);
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("ECOGUIDE 360 - demarrage");

  // HC-SR04
  pinMode(TRIG_PRESENCE,  OUTPUT);
  pinMode(ECHO_PRESENCE,  INPUT);
  pinMode(TRIG_PLASTIQUE, OUTPUT);
  pinMode(ECHO_PLASTIQUE, INPUT);
  pinMode(TRIG_PAPIER,    OUTPUT);
  pinMode(ECHO_PAPIER,    INPUT);
  pinMode(TRIG_ORGANIQUE, OUTPUT);
  pinMode(ECHO_ORGANIQUE, INPUT);

  // Boutons
  pinMode(BTN_PLASTIQUE, INPUT_PULLUP);
  pinMode(BTN_PAPIER,    INPUT_PULLUP);
  pinMode(BTN_ORGANIQUE, INPUT_PULLUP);

  // LED + buzzer
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_PIN, LOW);
  buzzerEteint();

  // I2C + LCD
  Wire.begin(I2C_SDA, I2C_SCL);
  scannerI2C();

  lcd.init();
  lcd.backlight();

  afficherMessage("ECOGUIDE 360", "Initialisation");
  delay(1200);

  // Servos
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  servoPlastique.setPeriodHertz(50);
  servoPapier.setPeriodHertz(50);
  servoOrganique.setPeriodHertz(50);

  servoPlastique.attach(SERVO_PLASTIQUE_PIN, SERVO_US_MIN, SERVO_US_MAX);
  servoPlastique.write(ANGLE_FERME);
  delay(300);

  servoPapier.attach(SERVO_PAPIER_PIN, SERVO_US_MIN, SERVO_US_MAX);
  servoPapier.write(ANGLE_FERME);
  delay(300);

  servoOrganique.attach(SERVO_ORGANIQUE_PIN, SERVO_US_MIN, SERVO_US_MAX);
  servoOrganique.write(ANGLE_FERME);
  delay(300);

  // WiFi
  connecterWiFi();

  // Première mesure + premier envoi
  mesurerNiveaux();
  envoyerDonnees();

  afficherMessage("ECOGUIDE 360", "Pret !");
  bipOK();
  delay(1200);
}


// ============================================================
// LOOP PRINCIPALE
// ============================================================

void loop()
{
  static int confirmationsPresence = 0;
  static int absencesConsecutives = 0;
  static bool cycleArme = true;   // true = un nouveau cycle peut démarrer
  static bool ecranMerci = false;
  static unsigned long dernierDebug = 0;
  static unsigned long dernierAffichage = 0;
  static bool afficherAlerte = false;

  verifierWiFi();

  // LED = connexion serveur OK
  digitalWrite(LED_PIN, (WiFi.status() == WL_CONNECTED && serveurOK) ? HIGH : LOW);


  // Présence confirmée sur 2 lectures consécutives (anti-parasites)
  if (detecterPresence())
  {
    absencesConsecutives = 0;

    if (confirmationsPresence < 5)
    {
      confirmationsPresence++;
    }
  }
  else
  {
    confirmationsPresence = 0;

    if (absencesConsecutives < 100)
    {
      absencesConsecutives++;
    }
  }

  presence = (confirmationsPresence >= 2);

  // Réarmement : la personne est partie (~0,6 s sans détection),
  // ou délai de sécurité écoulé si quelqu'un reste devant la station
  if (!cycleArme &&
      (absencesConsecutives >= 5 || millis() - dernierePresence >= DELAI_REARMEMENT_MS))
  {
    cycleArme = true;
  }

  if (DEBUG_PRESENCE && millis() - dernierDebug >= 1000)
  {
    dernierDebug = millis();

    Serial.printf("Presence : %.0f cm | detecte : %s | cycle arme : %s\n",
                  distancePresenceCm,
                  presence ? "oui" : "non",
                  cycleArme ? "oui" : "non");
  }


  // Personne détectée ET cycle armé : session de tri (une seule fois par passage)
  if (presence && cycleArme && millis() - dernierePresence > DELAI_PRESENCE)
  {
    afficherMessage("Bienvenue !", "Choisissez");
    bip(80);
    delay(600);

    gererTri();

    dernierePresence = millis();
    confirmationsPresence = 0;
    absencesConsecutives = 0;
    cycleArme = false;   // pas de nouveau cycle tant que la personne n'est pas partie
    dernierAffichage = millis();

    delay(500);
    return;
  }


  // Repos : personne détectée, ou personne déjà servie qui n'est pas encore partie
  // (pas de mesure/envoi pendant qu'un nouvel utilisateur attend son cycle)
  if (!presence || !cycleArme)
  {
    // Mesure + envoi périodiques (niveaux toujours à jour, même sans usage)
    if (millis() - dernierEnvoi >= INTERVALLE_ENVOI)
    {
      mesurerNiveaux();
      envoyerDonnees();
    }

    // Écran au repos
    if (presence && !cycleArme)
    {
      // personne déjà servie, encore devant la station
      if (!ecranMerci)
      {
        afficherMessage("Merci !", "Au revoir");
        ecranMerci = true;
      }
    }
    else if (ecranMerci)
    {
      // la personne est partie : retour immédiat à l'accueil
      ecranMerci = false;
      dernierAffichage = 0;
    }
    else if (millis() - dernierAffichage > 4000)
    {
      // accueil, avec alerte de remplissage en alternance
      dernierAffichage = millis();

      if (afficherAlerte && uneAlerteActive())
      {
        afficherMessage(
          "ALERTE : a vider",
          "Pl" + String(niveauPlastique) +
          " Pa" + String(niveauPapier) +
          " Or" + String(niveauOrganique)
        );
      }
      else
      {
        afficherMessage("ECOGUIDE 360", "Approchez-vous");
      }

      afficherAlerte = !afficherAlerte;
    }
  }

  delay(100);
}
