-- init_v2.sql：多租户改造——功能权限控制 + 用量统计
-- 在 ChatHttpServer 库上执行：mysql -uroot -p ChatHttpServer < init_v2.sql

-- 1) 用户表加功能级权限列（1=有权限 0=无权限，默认放开，按需收回）
--    can_chat  ：AI 对话（/chat/send、/chat/send-new-session）
--    can_image ：图像识别（/upload/send）
--    can_tts   ：语音合成（/chat/tts）
ALTER TABLE users
    ADD COLUMN can_chat  TINYINT NOT NULL DEFAULT 1,
    ADD COLUMN can_image TINYINT NOT NULL DEFAULT 1,
    ADD COLUMN can_tts   TINYINT NOT NULL DEFAULT 1;

-- 2) 聊天消息表加模型与 token 用量列（AI 回复行带用量，用户消息行填 0）
--    模型 API Key 仍统一由环境变量提供（DASHSCOPE_API_KEY / DOUBAO_API_KEY）
ALTER TABLE chat_message
    ADD COLUMN model             VARCHAR(32)  DEFAULT '',
    ADD COLUMN prompt_tokens     INT          DEFAULT 0,
    ADD COLUMN completion_tokens INT          DEFAULT 0;
