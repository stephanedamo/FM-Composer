#include "dataslider.hpp"
#include "../contextmenu/contextmenu.hpp"
#include "../popup/popup.hpp"
#include <math.h>
DataSlider* copiedSlider, pastedSlider;

DataSlider::DataSlider(int _x, int _y, int _max, int _min, string _name, int def, int _width, int _number) : tvalue(int2str[def], font, charSize), width(_width), name(_name, font, charSize)
, bg(Vector2f(_width - 2, 19 - 2)), x(_x), y(_y), value(clamp(def, _min, _max)), selected(false), vmax(_max), vmin(_min), number(_number), focused(false)
{
	name.setColor(colors[SLIDERTITLE]);
	setPosition(x, y);
	bg.setFillColor(colors[SLIDERBG]);
	tvalue.setColor(colors[SLIDERVALUE]);
	setValue(value);
	bg.setOutlineThickness(1);
}

bool DataSlider::update()
{
	if (contextMenu || !visible)
	{
		if (mouse.clickd)
			focused=false;
		return 0;
	}

	if (mouse.clickgReleased)
		selected = false;
	if (mouse.pos.x >= x && mouse.pos.x < x + width && mouse.pos.y >= y && mouse.pos.y < y + 20)
	{

		hovered = 1;
		if (mouse.clickg) {
			selected = true;
			focused=true;
		}
		if (mouse.clickd) {

			if (copiedSlider)
			{
				copiedSlider->focused=false;
			}
			copiedSlider = this;
			copiedSliderValue = value;


			if (popup->visible) {
				copyPasta->show(&popup->view);
			}
			else
			{
				copyPasta->show();
			}
			focused=true;
		}
		if (mouse.scroll && mouse.cursor == CURSOR_NORMAL)
		{
			setValue(value + mouse.scroll);
			focused=true;
			return true;
		}
		if (keyboard.plus)
		{
			setValue(value + 1);
			focused=true;
			return true;
		}
		if (keyboard.minus)
		{
			setValue(value - 1);
			focused=true;
			return true;
		}
		if (keyboard.multiply)
		{
			setValue(value *2);
			focused=true;
			return true;
		}
		if (keyboard.divide)
		{
			setValue(value / 2);
			focused=true;
			return true;
		}

	}
	else
	{
		if (mouse.clickg || mouse.clickd || mouse.scroll || keyboard.plus || keyboard.minus)
		{
			focused=false;
		}
		hovered = 0;
	}
	if (selected)
	{
		setValue(round((mouse.pos.x - x)*(float)(vmax - vmin) / width + vmin));
		return true;
	}
	return false;
}

bool DataSlider::updateOnRelease()
{
	updated = update();
	if (mouse.clickgReleased && updated)
	{
		updated = 0;
		return true;
	}
	return false;
}

void DataSlider::setPosition(int _x, int _y)
{
	x = _x;
	y = _y;
	bg.setPosition(x + 1, y + 1);
	bgValue.setPosition(x - min(vmin, 0)*width / (0.00001f + vmax - min(vmin, 0)), y);
	name.setPosition(x + 4, y);
	tvalue.setPosition((int)(x + width - tvalue.getLocalBounds().width - 2), y);
}

void DataSlider::draw()
{
	if (!visible)
		return;

	if (focused)
	{
		bg.setFillColor(colors[SLIDERBGHOVER]);
		bgValue.setFillColor(colors[SLIDERBARHOVER]);
		bg.setOutlineColor(colors[SLIDEROUTLINE]);

	}
	else
	{
		bgValue.setFillColor(colors[SLIDERBAR]);
		bg.setFillColor(colors[SLIDERBG]);
		bg.setOutlineColor(colors[SLIDERBG]);
	}
	window->draw(bg);
	window->draw(bgValue);
	window->draw(name);

	if (number > 0)
		window->draw(tvalue);
}

void DataSlider::setValue(int _value)
{
	value = clamp(_value, vmin, vmax);
	number == 2 && value >= 0 && value <= 128 ? tvalue.setString(noteName(value)) : tvalue.setString(std::to_string(value));
	int _width = (float)value / (vmax - min(vmin, 0))*width;
	if (_width == 0)
		_width = 1;
	bgValue.setSize(Vector2f(_width, 19));
	tvalue.setPosition((int)(x + width - tvalue.getLocalBounds().width - 2), y);
}

void DataSlider::setDisplayedValueOnly(string value)
{
	tvalue.setString(value);
	tvalue.setPosition((int)(x + width - tvalue.getLocalBounds().width - 2), y);
}

void DataSlider::setMinMax(int _min, int _max)
{
	vmin = _min;
	vmax = _max;
	setValue(value);
}

void DataSlider::setSize(int _width)
{
	width = _width;
	bg.setSize(Vector2f(_width - 2, 19 - 2));

	int barWidth = (float)value / (vmax - min(vmin, 0))*width;
	if (barWidth == 0)
		barWidth = 1;
	bgValue.setSize(Vector2f(barWidth, 19));
	tvalue.setPosition((int)(x + width - tvalue.getLocalBounds().width - 2), y);

}
