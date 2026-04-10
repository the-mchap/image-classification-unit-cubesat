"""SQLite image manifest. Tracks captured images with classification, confidence, and downlink status."""

import sqlite3
import os
import time
from typing import Optional, Tuple

from .. import config

logger = config.logging.get_logger(__name__)

__all__ = ["DataBase"]


class DataBase:
    """
    Manages MicroSD storage for images and a SQLite database for metadata indexing.
    Ensures that every saved image is tracked with its classification and status.
    """

    def __init__(self, db_path: str = config.app.DATABASE_PATH):
        """
        Initializes the database and ensures the images table exists.
        """
        self.db_path = db_path
        self._initialize_db()

    def _initialize_db(self):
        """Creates the images table if it doesn't already exist."""
        try:
            with sqlite3.connect(self.db_path) as conn:
                cursor = conn.cursor()
                cursor.execute("""
                    CREATE TABLE IF NOT EXISTS images (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        filename TEXT NOT NULL,
                        classification TEXT NOT NULL,
                        confidence REAL NOT NULL,
                        timestamp TEXT NOT NULL,
                        status INTEGER DEFAULT 0 -- 0=New, 1=Sent, 2=Deleted
                    )
                    """)
                conn.commit()
            logger.info(f"Database initialized at {self.db_path}")
        except sqlite3.Error as e:
            logger.error(f"Failed to initialize SQLite database: {e}")
            raise

    def save_image(
        self, image_data: bytes, classification: str, confidence: float
    ) -> bool:
        """
        Saves the image bytes to a file and updates the metadata database.
        Returns True if both operations succeed.
        """
        timestamp = time.strftime("%Y%m%d_%H%M%S")
        filename = f"img_{timestamp}_{classification.lower()}.jpg"
        file_path = os.path.join(config.app.STORAGE_DIR, filename)

        try:
            # 1. Save the JPEG file
            with open(file_path, "wb") as f:
                f.write(image_data)

            # 2. Update the SQLite manifest
            with sqlite3.connect(self.db_path) as conn:
                cursor = conn.cursor()
                cursor.execute(
                    """
                    INSERT INTO images (filename, classification, confidence, timestamp)
                    VALUES (?, ?, ?, ?)
                    """,
                    (filename, classification, confidence, timestamp),
                )
                conn.commit()

            logger.info(f"Image {filename} saved and indexed (Conf: {confidence:.2f})")
            return True

        except Exception as e:
            logger.error(f"Failed to save image {filename}: {e}")
            # If file was written but DB failed, try to cleanup the file
            if os.path.exists(file_path):
                os.remove(file_path)
            return False

    def get_next_for_downlink(self, classification: str) -> Optional[Tuple[int, str]]:
        """
        Finds the oldest unsent image of a specific classification.
        Returns (id, filename) or None.
        """
        try:
            with sqlite3.connect(self.db_path) as conn:
                cursor = conn.cursor()
                cursor.execute(
                    """
                    SELECT id, filename FROM images 
                    WHERE classification = ? AND status = 0 
                    ORDER BY timestamp ASC LIMIT 1
                    """,
                    (classification,),
                )
                return cursor.fetchone()
        except sqlite3.Error as e:
            logger.error(f"Failed to query next image for downlink: {e}")
            return None

    def get_image_by_index(
        self, classification: str, index: int
    ) -> Optional[Tuple[int, str]]:
        """
        Finds an image of a specific classification at the given 1-based index.
        Index is based on insertion order (ID).
        Returns (id, filename) or None.
        """
        if index < 1:
            return None

        try:
            with sqlite3.connect(self.db_path) as conn:
                cursor = conn.cursor()
                cursor.execute(
                    """
                    SELECT id, filename FROM images 
                    WHERE classification = ? 
                    ORDER BY id ASC LIMIT 1 OFFSET ?
                    """,
                    (classification, index - 1),
                )
                return cursor.fetchone()
        except sqlite3.Error as e:
            logger.error(
                f"Failed to query image at index {index} for {classification}: {e}"
            )
            return None

    def get_latest_image(self, classification: str) -> Optional[Tuple[int, str]]:
        """
        Finds the latest image of a specific classification.
        Returns (id, filename) or None.
        """
        try:
            with sqlite3.connect(self.db_path) as conn:
                cursor = conn.cursor()
                cursor.execute(
                    """
                    SELECT id, filename FROM images 
                    WHERE classification = ? 
                    ORDER BY timestamp DESC, id DESC LIMIT 1
                    """,
                    (classification,),
                )
                return cursor.fetchone()
        except sqlite3.Error as e:
            logger.error(f"Failed to query latest image for {classification}: {e}")
            return None

    def get_highest_confidence_image(
        self, classification: str
    ) -> Optional[Tuple[int, str]]:
        """
        Finds the image with the highest confidence for a specific classification.
        Returns (id, filename) or None.
        """
        try:
            with sqlite3.connect(self.db_path) as conn:
                cursor = conn.cursor()
                cursor.execute(
                    """
                    SELECT id, filename FROM images 
                    WHERE classification = ? 
                    ORDER BY confidence DESC, timestamp DESC LIMIT 1
                    """,
                    (classification,),
                )
                return cursor.fetchone()
        except sqlite3.Error as e:
            logger.error(
                f"Failed to query highest confidence image for {classification}: {e}"
            )
            return None

    def mark_as_sent(self, image_id: int):
        """Updates the status of an image to 1 (Sent)."""
        self._update_status(image_id, 1)

    def mark_as_deleted(self, image_id: int):
        """Updates the status of an image to 2 (Deleted)."""
        self._update_status(image_id, 2)

    def count_by_classification(self, classification: str) -> int:
        """
        Returns the number of images currently in the database for a given classification.
        """
        try:
            with sqlite3.connect(self.db_path) as conn:
                cursor = conn.cursor()
                cursor.execute(
                    "SELECT COUNT(*) FROM images WHERE classification = ?",
                    (classification,),
                )
                return cursor.fetchone()[0]
        except sqlite3.Error as e:
            logger.error(
                f"Failed to count images for classification {classification}: {e}"
            )
            return 0

    def get_unsent_counts(self) -> dict[str, int]:
        """
        Returns a dictionary with counts of unsent (status=0) images for each classification.
        """
        try:
            with sqlite3.connect(self.db_path) as conn:
                cursor = conn.cursor()
                cursor.execute(
                    "SELECT classification, COUNT(*) FROM images WHERE status = 0 GROUP BY classification"
                )
                rows = cursor.fetchall()
                # Initialize with zeros for expected classes
                counts = {"SKY": 0, "WATER": 0, "LAND": 0}
                for row in rows:
                    if row[0] in counts:
                        counts[row[0]] = row[1]
                return counts
        except sqlite3.Error as e:
            logger.error(f"Failed to get unsent counts: {e}")
            return {"SKY": 0, "WATER": 0, "LAND": 0}

    def _update_status(self, image_id: int, new_status: int):
        """Internal helper to update the status column."""
        try:
            with sqlite3.connect(self.db_path) as conn:
                cursor = conn.cursor()
                cursor.execute(
                    "UPDATE images SET status = ? WHERE id = ?",
                    (new_status, image_id),
                )
                conn.commit()
            logger.debug(f"Image ID {image_id} status updated to {new_status}")
        except sqlite3.Error as e:
            logger.error(f"Failed to update image status for ID {image_id}: {e}")
