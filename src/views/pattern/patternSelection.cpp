#include "patternSelection.hpp"
#include "../../gui/gui.hpp"
#include <math.h>

PatternSelection::PatternSelection() :
bg(Vector2f(COL_WIDTH, ROW_HEIGHT)),
moving(false),
deltaX(0),
deltaY(0)

{
	bg.setFillColor(colors[PATTERNSELECTION]);
	bg.setPosition(0, 0);

	moveRect.setOutlineThickness(1);
	moveRect.setOutlineColor(colors[MOVESELECTIONOUTLINE]);
	moveRect.setFillColor(Color(255, 255, 255, 0));

}

void PatternSelection::draw()
{
	window->draw(bg);
	if (moving)
		window->draw(moveRect);
}

bool PatternSelection::isHover(int x, int y)
{
	int x1, x2, y1, y2;
	getBounds(&x1, &x2, &y1, &y2);


	return x >= x1*COL_WIDTH
		&& y >= y1*ROW_HEIGHT
		&& x < x2*COL_WIDTH
		&& y < y2*ROW_HEIGHT;
}

void PatternSelection::getBounds(int *x1, int *x2, int *y1, int *y2)
{
	if (bg.getSize().x < 0)
	{
		*x2 = (int)round(bg.getPosition().x) / COL_WIDTH;
		*x1 = *x2 + (int)round(bg.getSize().x) / COL_WIDTH;
	}
	else
	{
		*x1 = (int)round(bg.getPosition().x) / COL_WIDTH;
		*x2 = *x1 + (int)round(bg.getSize().x) / COL_WIDTH;
	}

	if (bg.getSize().y < 0)
	{
		*y2 = (int)round(bg.getPosition().y) / ROW_HEIGHT;
		*y1 = *y2 + (int)round(bg.getSize().y) / ROW_HEIGHT;
	}
	else
	{
		*y1 = (int)round(bg.getPosition().y) / ROW_HEIGHT;
		*y2 = *y1 + (int)round(bg.getSize().y) / ROW_HEIGHT;
	}

	if (*x1 < 0)
	{
		*x2 -= *x1;
		*x1 = 0;
	}

	if (*y1 < 0)
	{
		*y2 -= *y1;
		*y1 = 0;
	}

}



void PatternSelection::resizeRelative(int x, int y)
{



	int oldSizeX = (int)round(bg.getSize().x) / COL_WIDTH;
	int oldSizeY = (int)round(bg.getSize().y) / ROW_HEIGHT;
	int oldPosX = (int)round(bg.getPosition().x) / COL_WIDTH;
	int oldPosY = (int)round(bg.getPosition().y) / ROW_HEIGHT;

	int newSizeX = clamp(oldSizeX + x, -oldPosX, 4 * FM_ch - oldPosX);
	int newSizeY = clamp(oldSizeY + y, -oldPosY, (int)fm->patternSize[fm->order] - oldPosY);

	int newPosX = oldPosX;
	int newPosY = oldPosY;

	if (oldSizeY > 0 && newSizeY <= 0)
	{

		newPosY++;
		newSizeY -= 2;

	}
	else if (oldSizeY<0 && newSizeY >= 0)
	{
		newPosY--;
		newSizeY += 2;
	}

	if (oldSizeX>0 && newSizeX <= 0)
	{

		newPosX++;
		newSizeX -= 2;

	}
	else if (oldSizeX < 0 && newSizeX >= 0)
	{
		newPosX--;
		newSizeX += 2;
	}

	if (newPosX + newSizeX<0)
	{
		newSizeX++;
	}
	else if (newPosX + newSizeX > 4 * FM_ch)
	{
		newSizeX--;
	}

	if (newPosY + newSizeY<0)
	{
		newSizeY++;
	}
	else if (newPosY + newSizeY >(int)fm->patternSize[fm->order])
	{
		newSizeY--;
	}

	bg.setSize(Vector2f(newSizeX*COL_WIDTH, newSizeY*ROW_HEIGHT));
	bg.setPosition(Vector2f(newPosX*COL_WIDTH, newPosY*ROW_HEIGHT));


}

void PatternSelection::resizeAbsolute(int xOrigin, int yOrigin, int x, int y)
{
	int mouseXpat2 = clamp(x / COL_WIDTH, 0, FM_ch * 4 - 1);
	int mouseYpat2 = clamp(y / ROW_HEIGHT, 0, (int)fm->patternSize[fm->order] - 1);



	if (mouseXpat2 < xOrigin)
	{
		if (deltaX != -COL_WIDTH)
		{
			bg.setPosition(bg.getPosition().x + COL_WIDTH, bg.getPosition().y);
		}
		deltaX = -COL_WIDTH;
	}
	else
	{
		if (deltaX == -COL_WIDTH)
		{
			bg.setPosition(bg.getPosition().x - COL_WIDTH, bg.getPosition().y);
		}
		deltaX = COL_WIDTH;
	}

	if (mouseYpat2 < yOrigin && yOrigin >0)
	{
		bg.setSize(Vector2f((int)(max<int>(1, min<int>(x, CH_WIDTH*FM_ch - 1)) - bg.getPosition().x) / COL_WIDTH*COL_WIDTH + deltaX, ((int)(max<int>(0, min<int>((int)fm->patternSize[fm->order] - 1, mouseYpat2))*ROW_HEIGHT - yOrigin*ROW_HEIGHT) / ROW_HEIGHT)*ROW_HEIGHT - ROW_HEIGHT));

		if (deltaY != ROW_HEIGHT)
		{
			deltaY = ROW_HEIGHT;
			bg.setPosition(bg.getPosition().x, yOrigin*ROW_HEIGHT + ROW_HEIGHT);
		}
	}
	else
	{
		bg.setSize(Vector2f((int)(max<int>(1, min<int>(x, CH_WIDTH*FM_ch - 1)) - bg.getPosition().x) / COL_WIDTH*COL_WIDTH + deltaX, ((int)(max<int>(0, min<int>((int)fm->patternSize[fm->order] - 1, mouseYpat2))*ROW_HEIGHT - yOrigin*ROW_HEIGHT) / ROW_HEIGHT)*ROW_HEIGHT + ROW_HEIGHT));

		if (deltaY != 0)
		{
			deltaY = 0;
			bg.setPosition(bg.getPosition().x, yOrigin*ROW_HEIGHT);
		}

	}
}


void PatternSelection::revertIfInverted()
{
	if ((int)bg.getSize().x < 0)
	{
		bg.setPosition(bg.getPosition().x + bg.getSize().x, bg.getPosition().y);
		bg.setSize(Vector2f(abs(bg.getSize().x), bg.getSize().y));

	}
	if ((int)bg.getSize().y < 0)
	{
		bg.setPosition(bg.getPosition().x, bg.getPosition().y + bg.getSize().y);
		bg.setSize(Vector2f(bg.getSize().x, abs(bg.getSize().y)));
	}
}

int PatternSelection::getChannel()
{
	return (int)round(bg.getPosition().x) / CH_WIDTH;
}
int PatternSelection::getRow()
{
	return (int)round(bg.getPosition().y) / ROW_HEIGHT;
}
int PatternSelection::getType()
{
	return ((int)round(bg.getPosition().x) / COL_WIDTH) % 4;
}
int PatternSelection::getMovedChannel()
{
	return moveRect.getPosition().x < 0 ? (int)round(moveRect.getPosition().x - CH_WIDTH*0.75) / CH_WIDTH : (int)round(moveRect.getPosition().x) / CH_WIDTH;
}
int PatternSelection::getMovedRow()
{
	return (int)round(moveRect.getPosition().y) / ROW_HEIGHT;
}
int PatternSelection::getMovedType()
{
	return moveRect.getPosition().x < 0 ? 0 : ((int)round(moveRect.getPosition().x) / COL_WIDTH) % 4;
}

bool PatternSelection::isSingle()
{
	return ((int)round(bg.getSize().x) == COL_WIDTH && (int)round(bg.getSize().y) == (int)ROW_HEIGHT);
}
