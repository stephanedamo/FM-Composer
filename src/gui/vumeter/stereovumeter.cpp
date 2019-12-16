#include "vumeter.hpp"
#include <math.h>

StereoVuMeter::StereoVuMeter(int x, int y) : vuLeft(x, y, "L"), vuRight(x + 60, y, "R")
{
	for (int i = 0; i < 5; i++)
	{
		dbBars[i] = RectangleShape(Vector2f(28, 1));
		dbValues[i].setFont(font);
		dbValues[i].setColor(colors[VUMETERTEXT]);
		dbValues[i].setCharacterSize(12);
		dbValues[i].setString(float2string(-(16 - i * 4), 2));

		dbBars[i].setFillColor(colors[VUMETERLINES]);
		int ypos = 50 + 128 - (pow(10, ((i + 1) / 5.0)) / 10 * 128);
		dbBars[i].setPosition(x + 32, ypos);
		dbValues[i].setPosition(x + 45 - (int)(dbValues[i].getLocalBounds().width / 2), ypos);
	}
	dbValues[5].setFont(font);
	dbValues[5].setColor(colors[VUMETERTEXT]);
	dbValues[5].setCharacterSize(12);
	dbValues[5].setString("dB");
	dbValues[5].setPosition(x + 45 - (int)(dbValues[5].getLocalBounds().width / 2), 50 + 128 + 2);
}
void StereoVuMeter::setValue(int left, int right)
{
	if (abs(left) > vuLeft.value)
		vuLeft.value = abs(left);
	if (abs(right) > vuRight.value)
		vuRight.value = abs(right);
}

void StereoVuMeter::update()
{
	vuLeft.update();
	vuRight.update();
}

void StereoVuMeter::draw()
{
	vuLeft.draw();
	vuRight.draw();

	for (int i = 0; i < 5; i++)
	{
		window->draw(dbBars[i]);
	}

	for (int i = 0; i < 6; i++)
	{
		window->draw(dbValues[i]);
	}
}
