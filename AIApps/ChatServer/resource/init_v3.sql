-- init_v3.sql：图像识别结果落库
-- 启动时 ChatServer::ensureImageResultTable() 也会 CREATE IF NOT EXISTS，
-- 本文件方便手工执行或对照表结构。
-- mysql -uroot -p ChatHttpServer < init_v3.sql

CREATE TABLE IF NOT EXISTS image_result (
    pk BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    user_id INT NOT NULL,
    username VARCHAR(50) NOT NULL DEFAULT '',
    filename VARCHAR(255) NOT NULL DEFAULT '',
    class_name VARCHAR(256) NOT NULL DEFAULT '',
    class_id INT NOT NULL DEFAULT -1,
    confidence FLOAT NOT NULL DEFAULT 0,
    model VARCHAR(64) NOT NULL DEFAULT '',
    ts BIGINT NOT NULL,
    KEY idx_image_result_user_ts (user_id, ts)
);
