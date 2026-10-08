import pyttsx3

engine = pyttsx3.init()

text = "Hello, Mersel! How can I assist you with your computer science and mathematics projects today?"

engine.setProperty('rate', 200)  # Speed of speech
engine.setProperty('volume', 1)  # Volume (0.0 to 1.0)

engine.say(text)

engine.runAndWait()
