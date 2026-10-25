import pygame
import sys
import random
import math
import time

# Initialize Pygame
pygame.init()

# Constants
SCREEN_WIDTH = 800
SCREEN_HEIGHT = 800
MAP_WIDTH = 3000  # Adjusted map width
MAP_HEIGHT = 2000  # Adjusted map height
FPS = 60
BOMB_RADIUS = 100
ENEMY_RADIUS = 20  # Updated to better fit the image size
PLAYER_RADIUS = 25
DRAGON_RADIUS = 25  # Increased size for the dragon
WALL_COLOR = (139, 69, 19)  # Brown color for walls
DOOR_COLOR = (150, 75, 0)
NUM_MAPS = 10  # Updated number of maps

# Set up the display
screen = pygame.display.set_mode((SCREEN_WIDTH, SCREEN_HEIGHT), pygame.FULLSCREEN)
pygame.display.set_caption("Infinite Dungeon with Variety of Enemies and Treasure")

# Load images
enemy_image = pygame.image.load("D:\\goblin.png")  # Replace with the actual path to your enemy image
enemy_image = pygame.transform.scale(enemy_image, (ENEMY_RADIUS * 2, ENEMY_RADIUS * 2))  # Scale the image to the desired size

player_image = pygame.image.load("D:\\hero.png")  # Replace with the actual path to your player image
player_image = pygame.transform.scale(player_image, (PLAYER_RADIUS * 2, PLAYER_RADIUS * 2))  # Scale the image to the desired size

dragon_image = pygame.image.load("D:\\dragon.png")  # Replace with the actual path to your dragon image
dragon_image = pygame.transform.scale(dragon_image, (DRAGON_RADIUS * 2, DRAGON_RADIUS * 2))  # Scale the image to the desired size

# Load images for new enemies
fast_enemy_image = pygame.image.load("D:\\skelly.png")  # Replace with the actual path to your fast enemy image
fast_enemy_image = pygame.transform.scale(fast_enemy_image, (ENEMY_RADIUS * 2, ENEMY_RADIUS * 2))  # Scale the image to the desired size

strong_enemy_image = pygame.image.load("D:\\strong_goblin.png")  # Replace with the actual path to your strong enemy image
strong_enemy_image = pygame.transform.scale(strong_enemy_image, (ENEMY_RADIUS * 2, ENEMY_RADIUS * 2))  # Scale the image to the desired size

# Load and scale the background image
background_image = pygame.image.load("D:\\floor.png").convert()    # Replace with the actual path to your strong enemy image
background_image = pygame.transform.scale(background_image, (MAP_WIDTH, MAP_HEIGHT))  # Scale the background image

# Wall class
class Wall:
    def __init__(self, x, y, width, height):
        self.rect = pygame.Rect(x, y, width, height)

    def draw(self, surface, camera):
        pygame.draw.rect(surface, WALL_COLOR, camera.apply(self.rect))

# Door class
class Door:
    def __init__(self, x, y, width, height):
        self.rect = pygame.Rect(x, y, width, height)
        self.open = False

    def toggle(self):
        self.open = not self.open

    def draw(self, surface, camera):
        color = (0, 255, 0) if self.open else DOOR_COLOR
        pygame.draw.rect(surface, color, camera.apply(self.rect))

# Dragon class
class Dragon:
    def __init__(self, x, y):
        self.rect = pygame.Rect(x, y, DRAGON_RADIUS * 2, DRAGON_RADIUS * 2)
        self.image = dragon_image
        self.speed = 15
        self.health = 100
        self.fire_active = False
        self.direction = (0, 0)
        self.fireballs = []

    def move(self, keys, walls, doors):
        original_position = self.rect.copy()
        direction_x, direction_y = 0, 0
        if keys[pygame.K_a]:
            self.rect.x -= self.speed
            direction_x = -1
        if keys[pygame.K_d]:
            self.rect.x += self.speed
            direction_x = 1
        if keys[pygame.K_w]:
            self.rect.y -= self.speed
            direction_y = -1
        if keys[pygame.K_s]:
            self.rect.y += self.speed
            direction_y = 1

        # Normalize direction vector
        if direction_x != 0 or direction_y != 0:
            length = math.hypot(direction_x, direction_y)
            self.direction = (direction_x / length, direction_y / length)

        for wall in walls:
            if self.rect.colliderect(wall.rect):
                self.rect = original_position

        for door in doors:
            if self.rect.colliderect(door.rect):
                self.rect = original_position

    def draw(self, surface, camera):
        surface.blit(self.image, camera.apply(self.rect))
        if self.fire_active:
            fire_length = 50
            fire_width = 20
            fire_x = self.rect.centerx + self.direction[0] * fire_length
            fire_y = self.rect.centery + self.direction[1] * fire_length
            fire_rect = pygame.Rect(fire_x, fire_y, fire_length, fire_width)
            pygame.draw.rect(surface, (255, 0, 0), camera.apply(fire_rect))
        for fireball in self.fireballs:
            fireball.draw(surface, camera)

    def breathe_fire(self):
        self.fire_active = True

    def shoot_fireball(self):
        fireball = Fireball(self.rect.centerx, self.rect.centery, self.direction, True)
        self.fireballs.append(fireball)

    def update_fireballs(self, walls, enemies):
        for fireball in self.fireballs[:]:
            fireball.update()
            if any(fireball.rect.colliderect(wall.rect) for wall in walls):
                self.fireballs.remove(fireball)
            else:
                for enemy in enemies[:]:
                    if fireball.rect.colliderect(enemy.rect):
                        print(f"Fireball hit enemy at {enemy.rect.topleft}")
                        if enemy.take_damage(20):  # Apply 20 damage
                            enemies.remove(enemy)
                        self.fireballs.remove(fireball)
                        break

# Fireball class
class Fireball:
    def __init__(self, x, y, direction, is_player_fireball=False):
        self.rect = pygame.Rect(x, y, 20, 20)
        self.color = (255, 69, 0)  # Orange color for fireball
        self.speed = 20
        self.direction = direction
        self.is_player_fireball = is_player_fireball

    def update(self):
        self.rect.x += self.speed * self.direction[0]
        self.rect.y += self.speed * self.direction[1]

    def draw(self, surface, camera):
        pygame.draw.rect(surface, self.color, camera.apply(self.rect))

# Player class
class Player:
    def __init__(self, x, y):
        self.rect = pygame.Rect(x, y, PLAYER_RADIUS * 2, PLAYER_RADIUS * 2)
        self.image = player_image
        self.speed = 10
        self.health = 50
        self.max_health = 100  # Added max health
        self.armor = 0  # Initialize armor
        self.max_armor = 100  # Added max armor
        self.loot = []
        self.sword_active = False
        self.weapon = 'bullet'  # Start with bullet as the primary weapon
        self.direction = (0, 0)  # Add direction attribute
        self.is_dragon = False
        self.transformation_time = 0
        self.cooldown = 0
        self.dragon = Dragon(x, y)
        self.shoot_cooldown = 0  # Initialize shoot cooldown

    # Add method to handle collecting loot
    def collect_loot(self, loot_item):
        if loot_item.item_type == 'health_potion':
            self.health = min(self.health + 20, self.max_health)  # Increase health by 20, up to max health
            print(f"Collected health potion. Health: {self.health}")
        elif loot_item.item_type == 'armor':
            self.armor = min(self.armor + 20, self.max_armor)  # Increase armor by 20, up to max armor
            print(f"Collected armor. Armor: {self.armor}")
        else:
            self.loot.append(loot_item.item_type)
            print(f"Collected {loot_item.item_type}.")

    def move(self, keys, walls, doors):
        if self.is_dragon:
            self.dragon.move(keys, walls, doors)
            self.revert_to_player()
        else:
            original_position = self.rect.copy()
            direction_x, direction_y = 0, 0
            if keys[pygame.K_a]:
                self.rect.x -= self.speed
                direction_x = -1
            if keys[pygame.K_d]:
                self.rect.x += self.speed
                direction_x = 1
            if keys[pygame.K_w]:
                self.rect.y -= self.speed
                direction_y = -1
            if keys[pygame.K_s]:
                self.rect.y += self.speed
                direction_y = 1

            # Normalize direction vector
            if direction_x != 0 or direction_y != 0:
                length = math.hypot(direction_x, direction_y)
                self.direction = (direction_x / length, direction_y / length)

            for wall in walls:
                if self.rect.colliderect(wall.rect):
                    self.rect = original_position

            for door in doors:
                if self.rect.colliderect(door.rect):
                    if keys[pygame.K_m]:
                        door.toggle()
                    else:
                        self.rect = original_position

            if keys[pygame.K_SPACE]:
                self.sword_active = True
            else:
                self.sword_active = False

    def switch_weapon(self):
        if self.weapon == 'bullet':
            self.weapon = 'sword'
        elif self.weapon == 'sword':
            self.weapon = 'black_hole'
        else:
            self.weapon = 'bullet'

    def transform_into_dragon(self):
        if not self.is_dragon and self.cooldown <= 0:
            self.is_dragon = True
            self.transformation_time = time.time()
            self.cooldown = 20 * FPS  # 1 minute cooldown converted to frames
            self.dragon.rect.topleft = self.rect.topleft
            print(f"Transformed into dragon at {self.transformation_time}")

    def revert_to_player(self):
        if self.is_dragon and time.time() - self.transformation_time >= 60:
            self.is_dragon = False
            self.rect.topleft = self.dragon.rect.topleft  # Update player's position to dragon's position
            self.cooldown = 20 * FPS  # Cooldown starts after reverting
            print(f"Reverted to player at {time.time()}")

    def update_cooldown(self):
        if self.cooldown > 0:
            self.cooldown -= 1  # Reduce cooldown by 1 frame
            print(f"Cooldown: {self.cooldown}")

    def shoot(self):
        if self.shoot_cooldown == 0:
            angle = math.atan2(self.direction[1], self.direction[0])
            bullet = Bullet(self.rect.centerx, self.rect.centery, angle)
            bullets.append(bullet)
            self.shoot_cooldown = 10  # Cooldown time for shooting

    def draw(self, surface, camera):
        if self.is_dragon:
            self.dragon.draw(surface, camera)
        else:
            surface.blit(self.image, camera.apply(self.rect))
            if self.sword_active and self.weapon == 'sword':
                sword_length = 40
                sword_width = 10
                sword_x = self.rect.centerx + self.direction[0] * sword_length
                sword_y = self.rect.centery + self.direction[1] * sword_length
                sword_rect = pygame.Rect(sword_x, sword_y, sword_length, sword_width)
                pygame.draw.rect(surface, (255, 255, 255), camera.apply(sword_rect))
                # Check collision with enemies and enemy dragon
                for enemy in enemies[:]:
                    if sword_rect.colliderect(enemy.rect):
                        if isinstance(enemy, EnemyDragon):
                            if enemy.take_damage(20):  # Apply 20 damage
                                enemies.remove(enemy)
                        else:
                            if enemy.take_damage(20):  # Apply 20 damage
                                enemies.remove(enemy)
            elif self.weapon == 'black_hole':
                pygame.draw.circle(surface, (0, 0, 255), camera.apply_pos(self.rect.center), 55)  # Blue outline
                pygame.draw.circle(surface, (0, 0, 0), camera.apply_pos(self.rect.center), 50)  # Black attack
# Bullet class
class Bullet:
    def __init__(self, x, y, angle):
        self.rect = pygame.Rect(x, y, 5, 5)
        self.color = (255, 0, 0)
        self.speed = 10
        self.angle = angle

    def update(self):
        self.rect.x += self.speed * math.cos(self.angle)
        self.rect.y += self.speed * math.sin(self.angle)

    def draw(self, surface, camera):
        pygame.draw.rect(surface, self.color, camera.apply(self.rect))

# Black Hole class
class BlackHole:
    def __init__(self, x, y):
        self.rect = pygame.Rect(x - 50, y - 50, 100, 100)
        self.color = (0, 0, 0)  # Black color for the attack
        self.outline_color = (0, 0, 255)  # Blue color for the outline
        self.active = True
        self.creation_time = time.time()

    def update(self, enemies):
        if time.time() - self.creation_time > 5:  # Black hole lasts for 5 seconds
            self.active = False
        if self.active:
            for enemy in enemies:
                direction_x = self.rect.centerx - enemy.rect.centerx
                direction_y = self.rect.centery - enemy.rect.centery
                distance = math.hypot(direction_x, direction_y)
                if distance > 0:
                    direction_x /= distance
                    direction_y /= distance
                enemy.rect.x += direction_x * 5
                enemy.rect.y += direction_y * 5

    def draw(self, surface, camera):
        if self.active:
            # Draw the blue outline
            pygame.draw.circle(surface, self.outline_color, camera.apply_pos(self.rect.center), 55)
            # Draw the black attack
            pygame.draw.circle(surface, self.color, camera.apply_pos(self.rect.center), 50)

# Base Enemy class
class Enemy:
    def __init__(self, x, y, image):
        self.rect = pygame.Rect(x, y, ENEMY_RADIUS * 2, ENEMY_RADIUS * 2)
        self.image = image
        self.speed = 1
        self.target = None
        self.loot = random.choice(['gold', 'silver', 'health_potion'])
        self.sword_active = True  # Give sword to all goblins
        self.direction = (0, 0)
        self.health = 50  # Add health attribute

    def move_towards(self, target, walls):
        if self.target is None:
            self.target = target

        direction_x = self.target.rect.centerx - self.rect.centerx
        direction_y = self.target.rect.centery - self.rect.centery
        distance_to_player = math.hypot(direction_x, direction_y)

        if distance_to_player > 0:
            direction_x /= distance_to_player
            direction_y /= distance_to_player

        future_rect = self.rect.move(direction_x * self.speed, direction_y * self.speed)
        if not any(future_rect.colliderect(wall.rect) for wall in walls):
            self.rect.x += direction_x * self.speed
            self.rect.y += direction_y * self.speed
        else:
            alternative_directions = [
                (direction_y, -direction_x),
                (-direction_y, direction_x),
            ]
            for alt_dir_x, alt_dir_y in alternative_directions:
                future_rect_alt = self.rect.move(alt_dir_x * self.speed, alt_dir_y * self.speed)
                if not any(future_rect_alt.colliderect(w.rect) for w in walls):
                    self.rect.x += alt_dir_x * self.speed
                    self.rect.y += alt_dir_y * self.speed
                    break

        if distance_to_player < 10:
            self.rect.x -= direction_x * self.speed
            self.rect.y -= direction_y * self.speed

        # Update direction
        if direction_x != 0 or direction_y != 0:
            self.direction = (direction_x, direction_y)

    def take_damage(self, amount):
        self.health -= amount
        print(f"Enemy took {amount} damage, health now {self.health}")
        if self.health <= 0:
            print("Enemy destroyed")
            return True  # Indicate that the enemy is destroyed
        return False

    def draw(self, surface, camera):
        surface.blit(self.image, camera.apply(self.rect))
        if self.sword_active:
            sword_length = 40
            sword_width = 10
            sword_x = self.rect.centerx + self.direction[0] * sword_length
            sword_y = self.rect.centery + self.direction[1] * sword_length
            sword_rect = pygame.Rect(sword_x, sword_y, sword_length, sword_width)
            pygame.draw.rect(surface, (255, 255, 255), camera.apply(sword_rect))

# Derived enemy classes
class FastEnemy(Enemy):
    def __init__(self, x, y):
        super().__init__(x, y, fast_enemy_image)
        self.speed = 2

class StrongEnemy(Enemy):
    def __init__(self, x, y):
        super().__init__(x, y, strong_enemy_image)
        self.health = 100

# Enemy Dragon class
class EnemyDragon:
    def __init__(self, x, y):
        self.rect = pygame.Rect(x, y, DRAGON_RADIUS * 2, DRAGON_RADIUS * 2)
        self.image = pygame.transform.scale(dragon_image, (DRAGON_RADIUS * 2, DRAGON_RADIUS * 2))
        self.speed = 5  # Slower speed for enemy dragon
        self.health = 100
        self.loot = random.choice(['gold', 'silver', 'health_potion'])
        self.fireballs = []
        self.direction = (0, 0)

    def move_towards(self, target, walls):
        direction_x = target.rect.centerx - self.rect.centerx
        direction_y = target.rect.centery - self.rect.centery
        distance_to_target = math.hypot(direction_x, direction_y)

        if distance_to_target > 0:
            direction_x /= distance_to_target
            direction_y /= distance_to_target

        self.rect.x += direction_x * self.speed
        self.rect.y += direction_y * self.speed

        for wall in walls:
            if self.rect.colliderect(wall.rect):
                self.rect.x -= direction_x * self.speed
                self.rect.y -= direction_y * self.speed

        self.direction = (direction_x, direction_y)

    def shoot_fireball(self):
        fireball = Fireball(self.rect.centerx, self.rect.centery, self.direction)
        self.fireballs.append(fireball)

    def take_damage(self, amount):
        self.health -= amount
        if self.health <= 0:
            return True  # Indicate that the enemy dragon is destroyed
        return False

    def draw(self, surface, camera):
        surface.blit(self.image, camera.apply(self.rect))
        for fireball in self.fireballs:
            fireball.draw(surface, camera)

# Loot class to represent loot items
class Loot:
    def __init__(self, x, y, item_type):
        self.rect = pygame.Rect(x, y, 20, 20)
        self.item_type = item_type
        self.color = {
            'gold': (255, 255, 0),
            'silver': (192, 192, 192),
            'health_potion': (255, 0, 0),
            'armor': (0, 255, 0)
        }[item_type]

    def draw(self, surface, camera):
        pygame.draw.rect(surface, self.color, camera.apply(self.rect))

# Bomb class
class Bomb:
    def __init__(self, x, y):
        self.rect = pygame.Rect(x, y, BOMB_RADIUS * 2, BOMB_RADIUS * 2)
        self.color = (255, 255, 0)
        self.active = True
        self.hover_start_time = None  # Track the hover start time

    def update(self, player):
        if self.rect.colliderect(player.rect):
            if self.hover_start_time is None:
                self.hover_start_time = time.time()  # Start the hover timer
            elif time.time() - self.hover_start_time >= 2:  # Check if hovering for 2 seconds
                return True
        else:
            self.hover_start_time = None  # Reset the hover timer if not hovering
        return False

    def draw(self, surface, camera):
        if self.active:
            pygame.draw.circle(surface, self.color, camera.apply_pos(self.rect.center), BOMB_RADIUS)

def is_valid_bomb_position(x, y, walls):
    bomb_rect = pygame.Rect(x, y, BOMB_RADIUS * 2, BOMB_RADIUS * 2)
    return not any(bomb_rect.colliderect(wall.rect) for wall in walls)

# Initialize the bomb in a valid position
def initialize_bomb(walls):
    while True:
        x = random.randint(0, MAP_WIDTH - BOMB_RADIUS * 2)
        y = random.randint(0, MAP_HEIGHT - BOMB_RADIUS * 2)
        if is_valid_bomb_position(x, y, walls):
            return Bomb(x, y)

# Camera class to keep player always visible
class Camera:
    def __init__(self, width, height):
        self.camera = pygame.Rect(0, 0, width, height)
        self.width = width
        self.height = height

    def apply(self, entity):
        return entity.move(self.camera.topleft)

    def apply_pos(self, pos):
        return pos[0] + self.camera.topleft[0], pos[1] + self.camera.topleft[1]

    def update(self, target):
        x = -target.rect.centerx + int(SCREEN_WIDTH / 2)
        y = -target.rect.centery + int(SCREEN_HEIGHT / 2)

        x = min(0, x)
        y = min(0, y)
        x = max(-(self.width - SCREEN_WIDTH), x)
        y = max(-(self.height - SCREEN_HEIGHT), y)

        self.camera = pygame.Rect(x, y, self.width, self.height)

# Function to generate a dungeon map with walls and doors
def generate_maze(structure):
    walls = []
    doors = []

    for y, row in enumerate(structure):
        for x, cell in enumerate(row):
            if cell == "#":
                walls.append(Wall(x * 100, y * 100, 100, 100))
            elif cell == " ":
                if (x > 0 and row[x - 1] == "#") or (x < len(row) - 1 and row[x + 1] == "#"):
                    doors.append(Door(x * 100, y * 100, 100, 20))
                if (y > 0 and structure[y - 1][x] == "#") or (y < len(structure) - 1 and structure[y + 1][x] == "#"):
                    doors.append(Door(x * 100, y * 100, 20, 100))

    return walls, doors

# Function to generate the starting area
def generate_starting_area():
    walls = [
        Wall(50, 50, 700, 20),  # Top wall
        Wall(50, 530, 700, 20),  # Bottom wall
        Wall(50, 50, 20, 500),  # Left wall
        Wall(200, 200, 200, 20),  # Inner top wall
        Wall(600, 200, 1500, 20),  # Inner top wall
        Wall(750, 400, 1500, 20),  # Inner top wall
        Wall(600, 600, 1500, 20),  # Inner top wall
        Wall(600, 800, 1500, 20),  # Inner top wall
        Wall(600, 1000, 1500, 20),  # Inner top wall
        Wall(600, 1200, 1500, 20),  # Inner top wall
        Wall(200, 380, 400, 20),  # Inner bottom wall
        Wall(200, 200, 20, 200),  # Inner left wall
        Wall(580, 200, 20, 200),  # Inner right wall
        Wall(50, 50, 40, 5500),  # Left wall
        Wall(50, 50, 5500, 40),  # Top wall
        Wall(2795.5, 50, 40, 5500),  # Left wall
    ]

    doors = [
        Door(380, 200, 100, 20),  # Inner top door
        Door(380, 380, 100, 20),  # Inner bottom door
    ]

    return walls, doors

# Function to transition to the next map
def transition_to_next_map(current_map_index, player, enemy_positions):
    new_map_index = (current_map_index + 1) % NUM_MAPS
    structures = [
        [
            "######################",
            "#          #   #     #",
            "#### #######      ####",
            "#             ###    #",
            "# ####### ##         #",
            "#         ##     #####",
            "## ######  ####      #",
            "#         ##     #####",
            "# #############      #",
            "#               #    #",
            "################### ##"
        ],
        [
            "######################",
            "#     #              #",
            "####  #######  #######",
            "#                    #",
            "###  ############### #",
            "#                    #",
            "#################### #",
            "#                    #",
            "########### ##########",
            "#                    #",
            "#################### #",
            "#                    #",
            "########### ##########",
            "#                    #",
            "#################### #",
            "#                    #",
            "########### ##########",

        ],
        [
            "######################",
            "#       #######      #",
            "#       #            #",
            "#       #######      #",
            "#                    #",
            "#       #######      #",
            "#       #            #",
            "#       #######      #",
            "##### ################",
            "##### ################",
            "#         #          #",
            "# ############### ####",
            "#                    #",
            "####### ########### ##",
            "#                    #",
            "########### ######## #",
            "#                    #",
            "### ##################",


        ],
        [
            "##### ################",
            "#         #          #",
            "# ############### ####",
            "#                    #",
            "####### ########### ##",
            "#                    #",
            "########### ######## #",
            "#                    #",
            "###   ################",
            "##### ################",
            "#         #          #",
            "# ############### ####",
            "#                    #",
            "####### ########### ##",
            "#                    #",
            "########### ######## #",
            "#                    #",
            "### ##################",

        ],
        [
            "######################",
            "#                    #",
            "# ################### #",
            "#                    #",
            "####### ############ #",
            "#                    #",
            "# ################### #",
            "#                    #",
            "################## ###",
        ],
        [
            "######################",
            "#                    #",
            "#### ####### #########",
            "#                    #",
            "# ####### ####### ####",
            "#                    #",
            "### ### ## ###########",
            "#                    #",
            "################### ##",
        ],
        [
            "################## ###",
            "#                    #",
            "####### ###### #######",
            "#                    #",
            "# ### ###### #########",
            "#                    #",
            "### ######### ####### ",
            "#                    #",
            "############ #########",
            "#          #   #     #",
            "#### #######      ####",
            "#             ###    #",
            "# ####### ##         #",
            "#         ##     #####",
            "## ######  ####      #",
            "#         ##     #####",
            "# #############      #",
            "#               #    #",
            "################### ##"

        ],
        [
            "######################",
            "#                    #",
            "##### ##### ###### ###",
            "#                    #",
            "### ####### ##########",
            "#                    #",
            "##### ###### #########",
            "#                    #",
            "################# ####",
            "#          #   #     #",
            "#### #######      ####",
            "#             ###    #",
            "# ####### ##         #",
            "#         ##     #####",
            "## ######  ####      #",
            "#         ##     #####",
            "# #############      #",
            "#               #    #",
            "################### ##"

        ],
        [
            "######################",
            "#                    #",
            "# ###### ######## ####",
            "#                    #",
            "### ##### ######## ###",
            "#                    #",
            "###### ###############",
            "#                    #",
            "######## #############",
            "#          #   #     #",
            "#### #######      ####",
            "#             ###    #",
            "# ####### ##         #",
            "#         ##     #####",
            "## ######  ####      #",
            "#         ##     #####",
            "# #############      #",
            "#               #    #",
            "################### ##"

        ],
        [
            "######################",
            "#                    #",
            "# ###### ######## ####",
            "#                    #",
            "### ##### ######## ###",
            "#                    #",
            "###### ###############",
            "#                    #",
            "################# ####",
            "######################",
            "#          #   #     #",
            "#### #######      ####",
            "#             ###    #",
            "# ####### ##         #",
            "#         ##     #####",
            "## ######  ####      #",
            "#         ##     #####",
            "# #############      #",
            "#               #    #",
            "################### ##"

        ]
    ]
    walls, doors = generate_maze(structures[new_map_index])
    player.rect.topleft = (150, 150)
    enemies = [random.choice([Enemy(x, y, enemy_image), FastEnemy(x, y), StrongEnemy(x, y), EnemyDragon(x, y)]) for x, y in enemy_positions]
    loot_items = [Loot(random.randint(100, MAP_WIDTH - 100), random.randint(100, MAP_HEIGHT - 100), random.choice(['gold', 'silver', 'health_potion', 'armor'])) for _ in range(5)]
    bomb = initialize_bomb(walls)
    return new_map_index, walls, doors, enemies, loot_items, bomb

# Function to display the start menu
def show_start_menu(selected_option):
    screen.fill((0, 0, 0))
    font = pygame.font.Font(None, 74)
    title_text = font.render('Infinite Dungeon', True, (255, 255, 255))
    screen.blit(title_text, (SCREEN_WIDTH // 2 - title_text.get_width() // 2, SCREEN_HEIGHT // 2 - 200))

    font = pygame.font.Font(None, 36)
    options = ['Start', 'Quit']
    for i, option in enumerate(options):
        color = (255, 255, 255) if i != selected_option else (0, 255, 0)
        option_text = font.render(option, True, color)
        screen.blit(option_text, (SCREEN_WIDTH // 2 - option_text.get_width() // 2, SCREEN_HEIGHT // 2 - 100 + i * 50))

    pygame.display.flip()

# Function to reset the game state
def reset_game():
    global player, bullets, black_holes, current_map_index, walls, doors, enemies, loot_items, bomb, camera, score
    player = Player(player_start_x, player_start_y)
    bullets = []
    black_holes = []
    current_map_index = 0
    walls, doors = generate_starting_area()
    enemies = [random.choice([Enemy(x, y, enemy_image), FastEnemy(x, y), StrongEnemy(x, y), EnemyDragon(x, y)]) for x, y in enemy_start_positions]
    loot_items = [Loot(random.randint(100, MAP_WIDTH - 100), random.randint(100, MAP_HEIGHT - 100), random.choice(['gold', 'silver', 'health_potion', 'armor'])) for _ in range(5)]
    bomb = initialize_bomb(walls)
    camera = Camera(MAP_WIDTH, MAP_HEIGHT)
    score = 0

# Starting points for player and enemies
player_start_x, player_start_y = 150, 150
enemy_start_positions = [
    (1650, 1250),
    (1500, 925),
    (1400, 1400),
    (1300, 1500),
    (1450, 1300),
    (1650, 925),
    (1500, 1300),
    (1400, 1400),
    (1300, 1500),
    (1450, 925)

]

# Initialize selected menu option
selected_option = 0

# Create instances
player = None
bullets = []
black_holes = []
current_map_index = 0
walls, doors = generate_starting_area()  # Use the new starting area
enemies = [random.choice([Enemy(x, y, enemy_image), FastEnemy(x, y), StrongEnemy(x, y), EnemyDragon(x, y)]) for x, y in enemy_start_positions]
loot_items = [Loot(random.randint(100, MAP_WIDTH - 100), random.randint(100, MAP_HEIGHT - 100), random.choice(['gold', 'silver', 'health_potion', 'armor'])) for _ in range(5)]
bomb = initialize_bomb(walls)
camera = Camera(MAP_WIDTH, MAP_HEIGHT)

# Game loop
clock = pygame.time.Clock()
running = True
in_start_menu = True
score = 0
shoot_cooldown = 1

while running:
    if in_start_menu:
        show_start_menu(selected_option)
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            if event.type == pygame.KEYDOWN:
                if event.key == pygame.K_RETURN:
                    if selected_option == 0:
                        in_start_menu = False
                        reset_game()  # Initialize the game state
                    elif selected_option == 1:
                        running = False
                if event.key == pygame.K_ESCAPE:
                    running = False
                if event.key == pygame.K_UP:
                    selected_option = (selected_option - 1) % 2
                if event.key == pygame.K_DOWN:
                    selected_option = (selected_option + 1) % 2
    else:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            if event.type == pygame.KEYDOWN:
                if event.key == pygame.K_q:
                    player.switch_weapon()
                if event.key == pygame.K_f and player.weapon == 'black_hole':
                    black_hole_x = player.rect.centerx + player.direction[0] * 100
                    black_hole_y = player.rect.centery + player.direction[1] * 100
                    black_hole = BlackHole(black_hole_x, black_hole_y)
                    black_holes.append(black_hole)
                if event.key == pygame.K_1:
                    player.transform_into_dragon()
                if event.key == pygame.K_e and player.is_dragon:
                    player.dragon.shoot_fireball()

        keys = pygame.key.get_pressed()
        player.move(keys, walls, doors)
        player.update_cooldown()

        if player.is_dragon:
            player.dragon.move(keys, walls, doors)
            player.dragon.update_fireballs(walls, enemies)

        if keys[pygame.K_SPACE]:
            player.shoot()

        if player.shoot_cooldown > 0:
            player.shoot_cooldown -= 1

        for bullet in bullets[:]:
            bullet.update()

        for bullet in bullets[:]:
            for wall in walls:
                if bullet.rect.colliderect(wall.rect):
                    bullets.remove(bullet)
                    break

        for enemy in enemies:
            enemy.move_towards(player, walls)
            if isinstance(enemy, EnemyDragon):
                if random.random() < 0.01:
                    enemy.shoot_fireball()
                for fireball in enemy.fireballs[:]:
                    fireball.update()
                    if fireball.rect.colliderect(player.rect):
                        player.health -= 10
                        enemy.fireballs.remove(fireball)
                        if player.health <= 0:
                            in_start_menu = True
                            reset_game()
                    for wall in walls:
                        if fireball.rect.colliderect(wall.rect):
                            enemy.fireballs.remove(fireball)
                            break

        for bullet in bullets[:]:
            for enemy in enemies[:]:
                if bullet.rect.colliderect(enemy.rect):
                    bullets.remove(bullet)
                    if isinstance(enemy, EnemyDragon):
                        if enemy.take_damage(10):
                            enemies.remove(enemy)
                            player.loot.append(enemy.loot)
                            score += 1
                    else:
                        enemies.remove(enemy)
                        player.loot.append(enemy.loot)
                        score += 1
                    break

        if player.sword_active and player.weapon == 'sword':
            sword_rect = pygame.Rect(player.rect.centerx, player.rect.centery, 40, 10)
            for enemy in enemies[:]:
                if sword_rect.colliderect(enemy.rect):
                    enemies.remove(enemy)
                    player.loot.append(enemy.loot)
                    score += 1

        for loot in loot_items[:]:
            if player.rect.colliderect(loot.rect):
                player.collect_loot(loot)
                loot_items.remove(loot)

        for black_hole in black_holes[:]:
            black_hole.update(enemies)
            if not black_hole.active:
                black_holes.remove(black_hole)

        if bomb.update(player):
            current_map_index, walls, doors, enemies, loot_items, bomb = transition_to_next_map(current_map_index, player, enemy_start_positions)

        camera.update(player)

        # Draw the background image
        screen.blit(background_image, (0, 0))

        for wall in walls:
            wall.draw(screen, camera)
        for door in doors:
            door.draw(screen, camera)

        player.draw(screen, camera)
        for bullet in bullets:
            bullet.draw(screen, camera)
        for enemy in enemies:
            enemy.draw(screen, camera)
        for loot in loot_items:
            loot.draw(screen, camera)
        bomb.draw(screen, camera)
        for black_hole in black_holes:
            black_hole.draw(screen, camera)

        font = pygame.font.Font(None, 36)
        score_text = font.render(f'Score: {score}', True, (255, 255, 255))
        health_text = font.render(f'Health: {player.health}', True, (255, 255, 255))
        armor_text = font.render(f'Armor: {player.armor}', True, (255, 255, 255))  # Display armor
        loot_text = font.render(f'Loot: {", ".join(player.loot)}', True, (255, 255, 255))
        screen.blit(score_text, (10, 10))
        screen.blit(health_text, (10, 40))
        screen.blit(armor_text, (10, 70))  # Display armor
        screen.blit(loot_text, (10, 100))

        if player.health <= 0:
            game_over_text = font.render('Game Over', True, (255, 0, 0))
            screen.blit(game_over_text, (SCREEN_WIDTH // 2 - 50, SCREEN_HEIGHT // 2 - 20))
            pygame.display.flip()
            pygame.time.wait(2000)
            running = False

        pygame.display.flip()
        clock.tick(FPS)

pygame.quit()
sys.exit()
